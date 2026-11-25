/*
 * S2 Real Production Adapter Negative & Crypto Regression Suite.
 *
 * Compiles and exercises the ACTUAL production main/port_ble.c and
 * main/port_nvs.c code directly against deterministic NimBLE/FreeRTOS/NVS
 * host stubs and real components/tesla_key/ + nanopb codecs (both portable
 * host CI and real Mbed TLS 3.6.5 via -DPK_USE_MBEDTLS).
 *
 * Covers S2-REVIEW-FIX-ORDER.md:
 *   §1  Minimum Legacy Evidence x Purpose Table (display vs auth separation)
 *   §2A GATT immutable callback token pool & NimBLE procedure lifecycle
 *   §2B No degraded Delivered without semaphore; SentUnknown vs NotSent; flush fail notify
 *   §2C Fault attribution & evidence gate closure (bad auth drop, route/gen/deadline)
 *   §2D DriveState non-finite / conflicting speed invalid semantics + SOC preservation
 *   §2E Candidate RNG/keygen failure blocks AddKey (no zero pubkey, no quiet Step 3 timeout)
 */

#include "port_internal.h"
#include "diag_log.h"
#include "pk_plan.h"
#include "pk_session.h"
#include "pk_copy.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "universal_message.pb.h"
#include "signatures.pb.h"
#include "vcsec.pb.h"
#include "car_server.pb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Deterministic Host Stub Harness for NimBLE, FreeRTOS, Diag & Port Core
 * ========================================================================== */

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *id, const char *desc)
{
    if (cond) {
        g_pass++;
        printf("S2A|%-6s|PASS|%s\n", id, desc);
    } else {
        g_fail++;
        fprintf(stderr, "S2A|%-6s|FAIL|%s\n", id, desc);
    }
}

/* Simulated monotonic clock & event queue */
static pk_ms_t g_now_ms = 1000u;
static pk_app_t g_app;
static pk_ble_adapter_t g_ble;

#define TEST_RX_POOL_CAP 8
static uint8_t g_rx_pool[TEST_RX_POOL_CAP][PK_PROTO_MAX_FRAME_LEN];
static bool g_rx_pool_used[TEST_RX_POOL_CAP];

#define TEST_EV_Q_CAP 32
static port_event_t g_ev_q[TEST_EV_Q_CAP];
static size_t g_ev_q_head = 0;
static size_t g_ev_q_count = 0;

pk_ms_t port_now_ms(void) { return g_now_ms; }
pk_app_t *port_bound_app(void) { return &g_app; }
pk_ble_adapter_t *port_bound_ble(void) { return &g_ble; }

bool port_post_event(const port_event_t *ev)
{
    if (!ev || g_ev_q_count >= TEST_EV_Q_CAP) {
        return false;
    }
    g_ev_q[(g_ev_q_head + g_ev_q_count) % TEST_EV_Q_CAP] = *ev;
    g_ev_q_count++;
    return true;
}

int port_rx_buf_alloc_copy(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > PK_PROTO_MAX_FRAME_LEN) {
        return -1;
    }
    for (int i = 0; i < TEST_RX_POOL_CAP; ++i) {
        if (!g_rx_pool_used[i]) {
            g_rx_pool_used[i] = true;
            memcpy(g_rx_pool[i], data, len);
            return i;
        }
    }
    return -1;
}

const uint8_t *port_rx_buf_ptr(int idx)
{
    if (idx < 0 || idx >= TEST_RX_POOL_CAP || !g_rx_pool_used[idx]) {
        return NULL;
    }
    return g_rx_pool[idx];
}

void port_rx_buf_release(int idx)
{
    if (idx >= 0 && idx < TEST_RX_POOL_CAP) {
        g_rx_pool_used[idx] = false;
    }
}

void diag_event(uint8_t type, uint8_t domain, uint8_t action, int code, int aux)
{
    (void)type; (void)domain; (void)action; (void)code; (void)aux;
}
void diag_raw(uint8_t dir, uint8_t domain, uint8_t action, const uint8_t *data, size_t len)
{
    (void)dir; (void)domain; (void)action; (void)data; (void)len;
}

/* FreeRTOS Semaphore Stub */
static bool g_sem_fail_create = false;
static int g_sem_dummy_obj = 1;
static int g_sem_count = 0;

/* NimBLE GATT Write Stub: supports auto-ACK, delayed/held callbacks, chunk-N fail,
 * and firing stale/late callbacks on demand during a later write! */
typedef enum {
    GATT_MODE_AUTO_ACK = 0,
    GATT_MODE_HOLD_TIMEOUT = 1,       /* accept write rc=0, hold cb, advance clock to expire */
    GATT_MODE_FAIL_IMMEDIATE = 2,     /* return rc=-1 on chunk 0 */
    GATT_MODE_FAIL_SECOND_CHUNK = 3,  /* ACK chunk 0, timeout chunk 1 (SentUnknown!) */
    GATT_MODE_FIRE_STALE_THEN_TIMEOUT = 4 /* on write, fire a saved stale cb, then timeout */
} stub_gatt_mode_t;

typedef struct {
    bool valid;
    uint16_t conn_h;
    ble_gatt_attr_fn *cb;
    void *cb_arg;
} held_cb_t;

#define HELD_CB_CAP 16
static held_cb_t g_held_cbs[HELD_CB_CAP];
static size_t g_held_cb_count = 0;
static stub_gatt_mode_t g_gatt_mode = GATT_MODE_AUTO_ACK;
static int g_gatt_write_calls = 0;
static int g_gatt_chunk_idx_in_call = 0;
static int g_stale_fire_idx = -1;

static void stub_reset_held_cbs(void)
{
    memset(g_held_cbs, 0, sizeof(g_held_cbs));
    g_held_cb_count = 0;
    g_stale_fire_idx = -1;
}

static void stub_fire_held_cb(size_t idx, int att_status)
{
    if (idx >= g_held_cb_count || !g_held_cbs[idx].valid) {
        return;
    }
    struct ble_gatt_error err = { .status = att_status, .att_handle = 0x20 };
    ble_gatt_attr_fn *cb = g_held_cbs[idx].cb;
    void *arg = g_held_cbs[idx].cb_arg;
    uint16_t ch = g_held_cbs[idx].conn_h;
    g_held_cbs[idx].valid = false;
    if (cb) {
        cb(ch, &err, NULL, arg);
    }
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (g_sem_fail_create) {
        return NULL;
    }
    g_sem_count = 0;
    return &g_sem_dummy_obj;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    if (g_sem_count > 0) {
        g_sem_count--;
        return pdTRUE;
    }
    if (ticks > 0) {
        /* Advance simulated time by wait_ms to exercise timeout paths cleanly. */
        g_now_ms += (pk_ms_t)ticks;
    }
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    g_sem_count = 1;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) { g_now_ms += (pk_ms_t)ticks; }

struct ble_hs_cfg_s ble_hs_cfg;
static bool g_disc_active = false;

int ble_uuid_cmp(const ble_uuid_t *u1, const ble_uuid_t *u2) { (void)u1; (void)u2; return 0; }
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *s, uint8_t l)
{ (void)f; (void)s; (void)l; return -1; }
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *data, uint16_t max_len, uint16_t *len)
{
    uint16_t n = (om->om_len < max_len) ? om->om_len : max_len;
    memcpy(data, om->om_data, n);
    if (len) *len = n;
    return 0;
}
int ble_hs_id_infer_auto(int p, uint8_t *out) { (void)p; if (out) *out = 0; return 0; }
int ble_hs_util_ensure_addr(int p) { (void)p; return 0; }
int ble_gap_disc(uint8_t o, int32_t d, const struct ble_gap_disc_params *p, ble_gap_event_fn *cb, void *a)
{ (void)o; (void)d; (void)p; (void)cb; (void)a; g_disc_active = true; return 0; }
int ble_gap_disc_cancel(void) { g_disc_active = false; return 0; }
int ble_gap_disc_active(void) { return g_disc_active ? 1 : 0; }
int ble_gap_connect(uint8_t o, const ble_addr_t *pa, int32_t d, const void *cp, ble_gap_event_fn *cb, void *a)
{ (void)o; (void)pa; (void)d; (void)cp; (void)cb; (void)a; return 0; }
int ble_gap_terminate(uint16_t ch, uint8_t r) { (void)ch; (void)r; return 0; }
int ble_gap_update_params(uint16_t ch, const struct ble_gap_upd_params *p) { (void)ch; (void)p; return 0; }
int ble_gap_conn_rssi(uint16_t ch, int8_t *out) { (void)ch; if (out) *out = -55; return 0; }
int ble_gattc_exchange_mtu(uint16_t ch, ble_gatt_mtu_fn *cb, void *a) { (void)ch; (void)cb; (void)a; return 0; }
int ble_gattc_disc_svc_by_uuid(uint16_t ch, const ble_uuid_t *u, ble_gatt_disc_svc_fn *cb, void *a)
{ (void)ch; (void)u; (void)cb; (void)a; return 0; }
int ble_gattc_disc_all_chrs(uint16_t ch, uint16_t s, uint16_t e, ble_gatt_chr_fn *cb, void *a)
{ (void)ch; (void)s; (void)e; (void)cb; (void)a; return 0; }
int ble_gattc_disc_all_dscs(uint16_t ch, uint16_t s, uint16_t e, ble_gatt_dsc_fn *cb, void *a)
{ (void)ch; (void)s; (void)e; (void)cb; (void)a; return 0; }

int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle,
                         const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg)
{
    (void)attr_handle;
    g_gatt_write_calls++;
    if (g_gatt_mode == GATT_MODE_FAIL_IMMEDIATE) {
        return -1;
    }
    (void)data;
    (void)data_len;
    if (g_gatt_mode == GATT_MODE_AUTO_ACK) {
        if (cb) {
            struct ble_gatt_error err = { .status = 0, .att_handle = attr_handle };
            cb(conn_handle, &err, NULL, cb_arg);
        }
        return 0;
    }
    if (g_gatt_mode == GATT_MODE_FAIL_SECOND_CHUNK) {
        g_gatt_chunk_idx_in_call++;
        if (g_gatt_chunk_idx_in_call == 1) {
            if (cb) {
                struct ble_gatt_error err = { .status = 0, .att_handle = attr_handle };
                cb(conn_handle, &err, NULL, cb_arg);
            }
            return 0;
        }
        /* Chunk 2: record held cb and let deadline expire -> SentUnknown! */
        if (g_held_cb_count < HELD_CB_CAP) {
            g_held_cbs[g_held_cb_count++] = (held_cb_t){ true, conn_handle, cb, cb_arg };
        }
        return 0;
    }
    if (g_gatt_mode == GATT_MODE_FIRE_STALE_THEN_TIMEOUT) {
        /* Hold the current write's cb, AND fire an older stale cb right now! */
        if (g_held_cb_count < HELD_CB_CAP) {
            g_held_cbs[g_held_cb_count++] = (held_cb_t){ true, conn_handle, cb, cb_arg };
        }
        if (g_stale_fire_idx >= 0) {
            stub_fire_held_cb((size_t)g_stale_fire_idx, 0);
            g_stale_fire_idx = -1;
        }
        return 0;
    }
    /* GATT_MODE_HOLD_TIMEOUT: accept write rc=0, hold cb unresolved */
    if (g_held_cb_count < HELD_CB_CAP) {
        g_held_cbs[g_held_cb_count++] = (held_cb_t){ true, conn_handle, cb, cb_arg };
    }
    return 0;
}

esp_err_t nimble_port_init(void) { return ESP_OK; }
int nimble_port_stop(void) { return 0; }
esp_err_t nimble_port_deinit(void) { return ESP_OK; }
void nimble_port_run(void) {}
void nimble_port_freertos_init(void (*host_task)(void *))
{
    (void)host_task;
    if (ble_hs_cfg.sync_cb) {
        ble_hs_cfg.sync_cb();
    }
}
void nimble_port_freertos_deinit(void) {}

/* Include the REAL production port_ble.c directly into this test TU so every
 * production code path and internal invariant is exercised directly! */
#include "../main/port_ble.c"

/* RNG failure injection hook from pk_crypto.c (host tests only) */
extern void pk_test_set_rng_fail(bool fail);

/* ==========================================================================
 * Test Helpers: Simulated Vehicle (Real ECDH + HMAC + AES-128-GCM + nanopb)
 * ========================================================================== */

static const char *TEST_VIN = "5YJ3E1EA1PF000001";
static uint8_t g_veh_priv[32];
static uint8_t g_veh_pub[65];

static void drain_events(void)
{
    while (g_ev_q_count > 0) {
        port_event_t ev = g_ev_q[g_ev_q_head];
        g_ev_q_head = (g_ev_q_head + 1u) % TEST_EV_Q_CAP;
        g_ev_q_count--;
        port_ble_handle_event(&g_app, &ev, g_now_ms);
    }
}

static void simulate_ble_connect_ready(int slot, uint16_t mtu)
{
    struct ble_gap_event ev;
    memset(&ev, 0, sizeof(ev));
    s_target_slot = slot;
    ev.type = BLE_GAP_EVENT_CONNECT;
    ev.connect.status = 0;
    ev.connect.conn_handle = 1;
    ble_gap_event_cb(&ev, NULL);

    /* Complete discovery + MTU */
    s_tx_val_handle = 0x20;
    s_rx_val_handle = 0x22;
    s_peer_mtu = mtu;
    pk_gatt_tx_set_mtu(&s_gatt_tx, mtu);
    post_ble_connected(slot, s_conn_gen);
    drain_events();
}

static void simulate_ble_disconnect(void)
{
    struct ble_gap_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = BLE_GAP_EVENT_DISCONNECT;
    ev.disconnect.reason = 0x13;
    ble_gap_event_cb(&ev, NULL);
    drain_events();
}

static void inject_rx_wire(const uint8_t *wire, size_t wire_len)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    size_t flen = pk_proto_frame_wrap(wire, wire_len, framed, sizeof(framed));
    struct os_mbuf om = { .om_data = framed, .om_len = (uint16_t)flen };
    struct ble_gap_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = BLE_GAP_EVENT_NOTIFY_RX;
    ev.notify_rx.conn_handle = 1;
    ev.notify_rx.attr_handle = 0x22;
    ev.notify_rx.om = &om;
    ble_gap_event_cb(&ev, NULL);
    drain_events();
}

static size_t build_session_info_frame(uint8_t *out, size_t cap,
                                       pk_proto_domain_t domain,
                                       const uint8_t route[16],
                                       const uint8_t challenge_uuid[16],
                                       const uint8_t client_pub[65],
                                       const uint8_t epoch[16],
                                       uint32_t counter, uint32_t clock_time,
                                       bool bad_hmac,
                                       int attach_fault,
                                       const uint8_t *override_route)
{
    Signatures_SessionInfo si = Signatures_SessionInfo_init_zero;
    uint8_t si_buf[128];
    uint8_t skey[16];
    uint8_t tag[32];

    si.counter = counter;
    si.publicKey.size = 65;
    memcpy(si.publicKey.bytes, g_veh_pub, 65);
    memcpy(si.epoch, epoch, 16);
    si.clock_time = clock_time;
    si.status = Signatures_Session_Info_Status_SESSION_INFO_STATUS_OK;
    pb_ostream_t ss = pb_ostream_from_buffer(si_buf, sizeof(si_buf));
    if (!pb_encode(&ss, Signatures_SessionInfo_fields, &si)) {
        return 0;
    }

    if (!pk_proto_ecdh_derive_session_key(g_veh_priv, client_pub, skey)) {
        return 0;
    }
    pk_proto_session_info_msg_t msg = {
        .vin = TEST_VIN,
        .challenge = challenge_uuid,
        .challenge_len = 16,
        .encoded_info = si_buf,
        .encoded_info_len = ss.bytes_written,
    };
    if (!pk_proto_session_info_tag_compute(skey, &msg, tag)) {
        return 0;
    }
    if (bad_hmac) {
        tag[0] ^= 0xFFu;
    }

    UniversalMessage_RoutableMessage rm = UniversalMessage_RoutableMessage_init_zero;
    rm.has_to_destination = true;
    rm.to_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
    rm.to_destination.sub_destination.routing_address.size = 16;
    memcpy(rm.to_destination.sub_destination.routing_address.bytes,
           override_route ? override_route : route, 16);
    rm.has_from_destination = true;
    rm.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    rm.from_destination.sub_destination.domain = (UniversalMessage_Domain)domain;
    rm.which_payload = UniversalMessage_RoutableMessage_session_info_tag;
    rm.payload.session_info.size = (pb_size_t)ss.bytes_written;
    memcpy(rm.payload.session_info.bytes, si_buf, ss.bytes_written);
    rm.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
    rm.sub_sigData.signature_data.which_sig_type = Signatures_SignatureData_session_info_tag_tag;
    rm.sub_sigData.signature_data.sig_type.session_info_tag.tag.size = 32;
    memcpy(rm.sub_sigData.signature_data.sig_type.session_info_tag.tag.bytes, tag, 32);
    rm.request_uuid.size = 16;
    memcpy(rm.request_uuid.bytes, challenge_uuid, 16);
    if (attach_fault != 0) {
        rm.has_signedMessageStatus = true;
        rm.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
        rm.signedMessageStatus.signed_message_fault = (UniversalMessage_MessageFault_E)attach_fault;
    }
    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    return pb_encode(&os, UniversalMessage_RoutableMessage_fields, &rm) ? os.bytes_written : 0;
}

static size_t build_routable_resp(uint8_t *out, size_t cap,
                                  pk_proto_domain_t domain,
                                  const uint8_t route[16],
                                  const uint8_t *req_uuid,
                                  const uint8_t *inner, size_t inner_len,
                                  const pk_proto_session_t *sess,
                                  const uint8_t req_id[17],
                                  bool encrypt, uint32_t resp_counter,
                                  bool tamper_tag, int attach_fault)
{
    UniversalMessage_RoutableMessage rm = UniversalMessage_RoutableMessage_init_zero;
    rm.has_from_destination = true;
    rm.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    rm.from_destination.sub_destination.domain = (UniversalMessage_Domain)domain;
    if (route) {
        rm.has_to_destination = true;
        rm.to_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
        rm.to_destination.sub_destination.routing_address.size = 16;
        memcpy(rm.to_destination.sub_destination.routing_address.bytes, route, 16);
    }
    if (req_uuid) {
        rm.request_uuid.size = 16;
        memcpy(rm.request_uuid.bytes, req_uuid, 16);
    }
    if (attach_fault != 0) {
        rm.has_signedMessageStatus = true;
        rm.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
        rm.signedMessageStatus.signed_message_fault = (UniversalMessage_MessageFault_E)attach_fault;
    }
    if (inner && inner_len > 0) {
        rm.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
        if (encrypt && sess) {
            uint8_t nonce[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };
            uint8_t meta[128];
            uint8_t aad[32];
            uint8_t tag[16];
            size_t mlen = 0;
            uint8_t flags_be[4] = { 0, 0, 0, 0 };
            uint8_t fault_be[4] = {
                (uint8_t)((uint32_t)attach_fault >> 24), (uint8_t)((uint32_t)attach_fault >> 16),
                (uint8_t)((uint32_t)attach_fault >> 8),  (uint8_t)((uint32_t)attach_fault)
            };
            uint8_t ctr_be[4] = {
                (uint8_t)(resp_counter >> 24), (uint8_t)(resp_counter >> 16),
                (uint8_t)(resp_counter >> 8),  (uint8_t)(resp_counter)
            };
            uint8_t sig_type = 0x09u; /* SIGNATURE_TYPE_AES_GCM_RESPONSE */
            uint8_t dom_b = (uint8_t)domain;
            uint8_t pers[17];
            memcpy(pers, TEST_VIN, 17);

            /* Build official protocol.md Response Metadata TLV (tags 0,1,2,5,7,8,9,0xFF) */
            meta[mlen++] = 0x00u; meta[mlen++] = 1; meta[mlen++] = sig_type;
            meta[mlen++] = 0x01u; meta[mlen++] = 1; meta[mlen++] = dom_b;
            meta[mlen++] = 0x02u; meta[mlen++] = 17; memcpy(meta + mlen, pers, 17); mlen += 17;
            meta[mlen++] = 0x05u; meta[mlen++] = 4; memcpy(meta + mlen, ctr_be, 4); mlen += 4;
            meta[mlen++] = 0x07u; meta[mlen++] = 4; memcpy(meta + mlen, flags_be, 4); mlen += 4;
            meta[mlen++] = 0x08u; meta[mlen++] = 17; memcpy(meta + mlen, req_id, 17); mlen += 17;
            meta[mlen++] = 0x09u; meta[mlen++] = 4; memcpy(meta + mlen, fault_be, 4); mlen += 4;
            meta[mlen++] = 0xFFu;
            pk_proto_sha256(meta, mlen, aad);

            rm.payload.protobuf_message_as_bytes.size = (pb_size_t)inner_len;
            pk_proto_aes128_gcm_encrypt(sess->session_key, nonce, aad, 32,
                                        inner, inner_len,
                                        rm.payload.protobuf_message_as_bytes.bytes, tag);
            if (tamper_tag) {
                tag[0] ^= 0xAAu;
            }
            rm.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
            rm.sub_sigData.signature_data.which_sig_type = Signatures_SignatureData_AES_GCM_Response_data_tag;
            rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.nonce.size = 12;
            memcpy(rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.nonce.bytes, nonce, 12);
            rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.counter = resp_counter;
            rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag.size = 16;
            memcpy(rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag.bytes, tag, 16);
        } else {
            rm.payload.protobuf_message_as_bytes.size = (pb_size_t)inner_len;
            memcpy(rm.payload.protobuf_message_as_bytes.bytes, inner, inner_len);
        }
    }
    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    return pb_encode(&os, UniversalMessage_RoutableMessage_fields, &rm) ? os.bytes_written : 0;
}

static void setup_connected_slot0(uint16_t mtu, bool establish_sessions)
{
    pk_proto_key_slot_t *ks;
    uint8_t veh_seed[32];
    for (int i = 0; i < 32; ++i) veh_seed[i] = (uint8_t)(0x50 + i);

    g_now_ms = 1000u;
    g_sem_fail_create = false;
    g_gatt_mode = GATT_MODE_AUTO_ACK;
    g_gatt_write_calls = 0;
    g_gatt_chunk_idx_in_call = 0;
    g_ev_q_head = 0;
    g_ev_q_count = 0;
    memset(g_rx_pool_used, 0, sizeof(g_rx_pool_used));
    stub_reset_held_cbs();
    pk_test_set_rng_fail(false);

    /* Tear down cleanly before re-init */
    if (s_ble_stack_ready) {
        port_ble_deinit();
    }
    s_gatt_tx_sem = NULL;
    s_gatt_tok_exhausted_drops = 0;
    memset(s_gatt_tok_pool, 0, sizeof(s_gatt_tok_pool));

    pk_app_init(&g_app, PK_VEHICLE_NONE, true);
    pk_store_add_verified(&g_app.store, TEST_VIN, PK_STORE_CRASH_NONE);
    g_app.vehicle = 0;
    g_app.paired = true;
    g_app.awake = true;
    g_app.now = g_now_ms;

    port_ble_init_and_attach(&g_app, &g_ble);
    pk_proto_set_entropy_source(PK_ENTROPY_SRC_BLE, true);

    /* Ensure deterministic vehicle keypair + slot 0 keypair */
    pk_proto_p256_keygen(g_veh_priv, g_veh_pub, veh_seed);
    ks = port_slot_keys(0);
    memset(ks, 0, sizeof(*ks));
    port_ensure_slot_keypair(0);

    simulate_ble_connect_ready(0, mtu);

    if (establish_sessions) {
        uint8_t si_wire[256];
        uint8_t ep_v[16] = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };
        uint8_t ep_i[16] = { 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2 };
        /* Establish VCSEC session via handshake recorded on connect */
        size_t slen = build_session_info_frame(
            si_wire, sizeof(si_wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            s_session.handshake[0].routing_addr, s_session.handshake[0].uuid,
            ks->pubkey, ep_v, 10, 500, false, 0, NULL);
        inject_rx_wire(si_wire, slen);
        /* Establish Infotainment session (triggered automatically by VCSEC ok) */
        slen = build_session_info_frame(
            si_wire, sizeof(si_wire), PK_PROTO_DOMAIN_INFOTAINMENT,
            s_session.handshake[1].routing_addr, s_session.handshake[1].uuid,
            ks->pubkey, ep_i, 20, 600, false, 0, NULL);
        inject_rx_wire(si_wire, slen);
        /* Clear any background status probe so tests start clean */
        memset(s_wire_txns, 0, sizeof(s_wire_txns));
        g_ble.in_flight = false;
        g_gatt_write_calls = 0;
    }
}

int main(void)
{
    printf("=== S2 Real Production Adapter Negative & Crypto Regression Suite ===\n");

    /* ----------------------------------------------------------------------
     * SUITE A (§2A): GATT Immutable Callback Token Pool & NimBLE Lifecycle
     * ---------------------------------------------------------------------- */
    {
        uint8_t payload[60];
        memset(payload, 0x5A, sizeof(payload));

        /* A1: Normal multi-chunk delivery reclaims all token slots */
        setup_connected_slot0(23u, true); /* MTU=23 -> 20B chunks -> 4 chunks */
        g_gatt_mode = GATT_MODE_AUTO_ACK;
        pk_tx_outcome_t r1 = gatt_write_framed(payload, sizeof(payload), g_now_ms + 3000u);
        bool all_free = true;
        for (size_t i = 0; i < GATT_CB_TOK_POOL_CAP; ++i) {
            if (s_gatt_tok_pool[i].in_use) all_free = false;
        }
        check(r1 == PK_TX_DELIVERED && all_free,
              "A1", "Multi-chunk GATT write delivers & reclaims all callback token slots");

        /* A2: Cross-4-reuse late callback MUST NOT alias or complete new write */
        setup_connected_slot0(247u, true);
        g_gatt_mode = GATT_MODE_HOLD_TIMEOUT;
        pk_tx_outcome_t t1 = gatt_write_framed(payload, 10, g_now_ms + 200u);
        gatt_write_tok_t *tok1 = (gatt_write_tok_t *)g_held_cbs[0].cb_arg;
        uint32_t tok1_seq = tok1->write_seq;
        uint32_t tok1_gen = tok1->conn_gen;

        /* Perform 5 more writes (crossing 4+ reuses) while tok1 is still unresolved */
        g_gatt_mode = GATT_MODE_AUTO_ACK;
        for (int k = 0; k < 5; ++k) {
            (void)gatt_write_framed(payload, 10, g_now_ms + 500u);
        }
        bool tok1_intact = tok1->in_use && tok1->write_seq == tok1_seq && tok1->conn_gen == tok1_gen;

        /* Now start write #7 (pending) and fire stale tok1 callback DURING write #7! */
        g_gatt_mode = GATT_MODE_FIRE_STALE_THEN_TIMEOUT;
        g_stale_fire_idx = 0;
        pk_tx_outcome_t t7 = gatt_write_framed(payload, 10, g_now_ms + 200u);
        check(t1 == PK_TX_SENT_UNKNOWN && tok1_intact && t7 == PK_TX_SENT_UNKNOWN && !tok1->in_use,
              "A2", "Cross-4-reuse stale callback stays immutable, never completes write #7, and frees on completion");

        /* A3: Disconnect + reconnect + late callback from old conn_gen */
        setup_connected_slot0(247u, true);
        g_gatt_mode = GATT_MODE_HOLD_TIMEOUT;
        (void)gatt_write_framed(payload, 10, g_now_ms + 200u);
        gatt_write_tok_t *old_tok = (gatt_write_tok_t *)g_held_cbs[0].cb_arg;
        uint32_t old_gen = old_tok->conn_gen;
        simulate_ble_disconnect();
        /* Token MUST still remain in_use until NimBLE fires cb (no premature wipe on disconnect!) */
        bool preserved_across_disc = old_tok->in_use && old_tok->conn_gen == old_gen;
        simulate_ble_connect_ready(0, 247u);
        uint32_t new_gen = s_conn_gen;
        g_gatt_mode = GATT_MODE_FIRE_STALE_THEN_TIMEOUT;
        g_stale_fire_idx = 0;
        pk_tx_outcome_t tnew = gatt_write_framed(payload, 10, g_now_ms + 200u);
        check(preserved_across_disc && new_gen > old_gen && tnew == PK_TX_SENT_UNKNOWN && !old_tok->in_use,
              "A3", "Old conn_gen late callback after disconnect/reconnect never completes new write");

        /* A4: Bounded token pool exhaustion (8 held -> 9th fails explicitly without overwriting) */
        setup_connected_slot0(247u, true);
        g_gatt_mode = GATT_MODE_HOLD_TIMEOUT;
        for (size_t i = 0; i < GATT_CB_TOK_POOL_CAP; ++i) {
            (void)gatt_write_framed(payload, 10, g_now_ms + 100u);
        }
        uint32_t drops_before = s_gatt_tok_exhausted_drops;
        pk_tx_outcome_t t9 = gatt_write_framed(payload, 10, g_now_ms + 100u);
        /* Reclaim one slot by delivering held cb #0, then verify next write succeeds */
        stub_fire_held_cb(0, BLE_HS_ENOTCONN);
        g_gatt_mode = GATT_MODE_AUTO_ACK;
        pk_tx_outcome_t t10 = gatt_write_framed(payload, 10, g_now_ms + 500u);
        check(t9 == PK_TX_NOT_SENT && s_gatt_tok_exhausted_drops == drops_before + 1u &&
              t10 == PK_TX_DELIVERED,
              "A4", "Token pool exhaustion fails explicitly without overwrite and recovers on cb completion");

        /* A5: Immediate ble_gattc_write_flat error releases token slot (no leak) */
        setup_connected_slot0(247u, true);
        g_gatt_mode = GATT_MODE_FAIL_IMMEDIATE;
        pk_tx_outcome_t tf = gatt_write_framed(payload, 10, g_now_ms + 200u);
        bool all_free_err = true;
        for (size_t i = 0; i < GATT_CB_TOK_POOL_CAP; ++i) {
            if (s_gatt_tok_pool[i].in_use) all_free_err = false;
        }
        check(tf == PK_TX_NOT_SENT && all_free_err,
              "A5", "Immediate GATT write rc!=0 releases token slot with zero leak");
    }

    /* ----------------------------------------------------------------------
     * SUITE B (§2B): No Degraded Delivered + SentUnknown vs NotSent + Flush Fail
     * ---------------------------------------------------------------------- */
    {
        /* B1: Semaphore creation failure MUST fail closed (never Delivered!) */
        if (s_ble_stack_ready) port_ble_deinit();
        s_gatt_tx_sem = NULL;
        g_sem_fail_create = true;
        pk_app_init(&g_app, PK_VEHICLE_NONE, true);
        pk_store_add_verified(&g_app.store, TEST_VIN, PK_STORE_CRASH_NONE);
        g_app.vehicle = 0; g_app.paired = true; g_app.now = g_now_ms;
        port_ble_init_and_attach(&g_app, &g_ble);
        simulate_ble_connect_ready(0, 247u);
        g_gatt_write_calls = 0;
        uint8_t dummy[8] = { 1,2,3,4,5,6,7,8 };
        pk_tx_outcome_t nosem = gatt_write_framed(dummy, sizeof(dummy), g_now_ms + 500u);
        check(s_gatt_tx_sem == NULL && nosem == PK_TX_NOT_SENT && g_gatt_write_calls == 0,
              "B1", "Missing semaphore fails closed (PK_TX_NOT_SENT, 0 writes, never PK_TX_DELIVERED)");

        /* B2: Partial chunk sent -> PK_ERR_UNKNOWN / PK_TX_SENT_UNKNOWN vs 0 bytes -> NotSent */
        setup_connected_slot0(23u, true); /* MTU=23 forces multi-chunk signed VCSEC frame */
        pk_ble_frame_t fr;
        memset(&fr, 0, sizeof(fr));
        fr.domain = PK_DOMAIN_VCSEC;
        fr.action = PK_ACTION_LOCK;
        fr.txn_id = 101;
        fr.scope = pk_app_scope(&g_app);
        fr.deadline = g_now_ms + 500u;

        g_gatt_mode = GATT_MODE_FAIL_IMMEDIATE;
        pk_err_t e_notsent = g_app.hal.ble->send_once(g_app.hal.ble_ctx, &fr);
        bool ok_notsent = (e_notsent == PK_ERR_NOT_WIRED &&
                           g_ble.last_tx_outcome == PK_TX_NOT_SENT &&
                           g_app.last_tx_outcome == PK_TX_NOT_SENT);

        /* Re-establish VCSEC session for partial chunk 2 failure test */
        setup_connected_slot0(23u, true);
        fr.txn_id = 102;
        fr.scope = pk_app_scope(&g_app);
        fr.deadline = g_now_ms + 500u;
        g_gatt_mode = GATT_MODE_FAIL_SECOND_CHUNK;
        g_gatt_chunk_idx_in_call = 0;
        pk_err_t e_partial = g_app.hal.ble->send_once(g_app.hal.ble_ctx, &fr);
        bool ok_partial = (e_partial == PK_ERR_UNKNOWN &&
                           g_ble.last_tx_outcome == PK_TX_SENT_UNKNOWN &&
                           g_app.last_tx_outcome == PK_TX_SENT_UNKNOWN &&
                           !port_slot_keys(0)->vcsec.valid);
        check(ok_notsent && ok_partial,
              "B2", "Chunk-0 fail -> NotSent; chunk-1 fail -> SentUnknown/PK_ERR_UNKNOWN + session drop");

        /* B3: flush_waiting_session_txn failure notifies orchestration (no hanging pending!) */
        setup_connected_slot0(23u, false); /* connected, NO VCSEC session yet */
        g_app.now = g_now_ms;
        pk_intent_t lock_in = pk_app_make_intent(&g_app, PK_ACTION_LOCK);
        bool committed = pk_app_commit(&g_app, &lock_in, g_now_ms);
        bool queued_wait = committed && g_app.has_pending && s_wire_txns[0].waiting_session;
        /* Now deliver valid SessionInfo, while chunk 2 of flush fails -> SentUnknown */
        g_gatt_mode = GATT_MODE_FAIL_SECOND_CHUNK;
        g_gatt_chunk_idx_in_call = 0;
        uint8_t si_wire[256];
        uint8_t ep[16] = { 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9 };
        size_t slen = build_session_info_frame(
            si_wire, sizeof(si_wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            s_session.handshake[0].routing_addr, s_session.handshake[0].uuid,
            port_slot_keys(0)->pubkey, ep, 1, 100, false, 0, NULL);
        inject_rx_wire(si_wire, slen);
        bool notified_unknown = (!g_app.has_pending && !g_app.busy &&
                                 g_app.last_tx_outcome == PK_TX_SENT_UNKNOWN &&
                                 g_app.message == PK_MSG_LOCK_UNCONFIRMED);

        /* Also test flush failure on RNG error -> PK_TX_NOT_SENT + notified */
        setup_connected_slot0(247u, false);
        g_app.now = g_now_ms;
        lock_in = pk_app_make_intent(&g_app, PK_ACTION_LOCK);
        (void)pk_app_commit(&g_app, &lock_in, g_now_ms);
        pk_test_set_rng_fail(true);
        slen = build_session_info_frame(
            si_wire, sizeof(si_wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            s_session.handshake[0].routing_addr, s_session.handshake[0].uuid,
            port_slot_keys(0)->pubkey, ep, 2, 101, false, 0, NULL);
        inject_rx_wire(si_wire, slen);
        pk_test_set_rng_fail(false);
        bool notified_notsent = (!g_app.has_pending && !g_app.busy &&
                                 g_app.last_tx_outcome == PK_TX_NOT_SENT &&
                                 g_app.message == PK_MSG_CANCELLED);
        check(queued_wait && notified_unknown && notified_notsent,
              "B3", "flush_waiting_session_txn failure notifies orchestration with exact SentUnknown/NotSent");
    }

    /* ----------------------------------------------------------------------
     * SUITE C (§2C): Fault Attribution & Evidence Gate Closure
     * ---------------------------------------------------------------------- */
    {
        uint8_t wire[512];
        uint8_t wrong_route[16];
        memset(wrong_route, 0xEE, sizeof(wrong_route));

        /* C1: Bad SessionInfo HMAC + same-packet fault -> ZERO fault side effects */
        setup_connected_slot0(247u, true);
        pk_intent_t lock_in = pk_app_make_intent(&g_app, PK_ACTION_LOCK);
        (void)pk_app_commit(&g_app, &lock_in, g_now_ms);
        uint8_t ep_bad[16] = { 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3 };
        size_t wlen = build_session_info_frame(
            wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
            port_slot_keys(0)->pubkey, ep_bad, 99, 999, true /* bad HMAC! */,
            PK_PROTO_FAULT_INVALID_SIGNATURE, NULL);
        inject_rx_wire(wire, wlen);
        check(port_slot_keys(0)->vcsec.valid && g_app.has_pending && s_wire_txns[0].active,
              "C1", "Same-packet bad SessionInfo HMAC + fault dropped with ZERO side effects");

        /* C2: Bad AES-GCM tag + same-packet fault -> ZERO fault side effects */
        uint8_t dummy_inner[4] = { 0x08, 0x00, 0x00, 0x00 };
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   dummy_inner, sizeof(dummy_inner),
                                   &port_slot_keys(0)->vcsec, s_wire_txns[0].req_id,
                                   true, 0u, true /* tamper GCM tag! */,
                                   PK_PROTO_FAULT_INVALID_COUNTER);
        inject_rx_wire(wire, wlen);
        check(port_slot_keys(0)->vcsec.valid && g_app.has_pending && s_wire_txns[0].active,
              "C2", "Same-packet bad AES-GCM tag + fault dropped with ZERO side effects");

        /* C3: Valid SessionInfo + UNATTRIBUTED fault (wrong route) -> SessionInfo applies, fault blocked! */
        uint8_t saved_route[16];
        memcpy(saved_route, s_wire_txns[0].routing_addr, 16);
        uint8_t hs_uuid[16];
        memcpy(hs_uuid, s_wire_txns[0].uuid, 16);
        /* Note a handshake with valid route, then deliver valid SessionInfo + fault with WRONG route */
        pk_session_note_handshake_ex(&s_session, 0, s_conn_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     saved_route, hs_uuid, g_now_ms);
        /* Send valid SessionInfo with matching route? Wait: if override_route is wrong,
         * pk_session_apply checks route_matches_ref too. So test matching handshake route
         * on SessionInfo with a different txn route! */
        uint8_t hs_route[16];
        memset(hs_route, 0x77, 16);
        pk_session_note_handshake_ex(&s_session, 0, s_conn_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     hs_route, hs_uuid, g_now_ms);
        uint8_t ep_same[16] = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };
        wlen = build_session_info_frame(
            wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            hs_route, hs_uuid, port_slot_keys(0)->pubkey, ep_same, 15, 550, false,
            PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES, NULL);
        /* hs_route matches handshake (so SessionInfo applies!), but hs is now consumed
         * and txn has saved_route != hs_route -> fault MUST NOT clear live txn! */
        inject_rx_wire(wire, wlen);
        check(port_slot_keys(0)->vcsec.clock_time == 550u && g_app.has_pending && s_wire_txns[0].active,
              "C3", "Valid SessionInfo does NOT endorse unattributed outer fault (no fresh_session bypass)");

        /* C4: Unattributed outer fault (wrong route / missing route / wrong conn_gen) -> rejected */
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   wrong_route, s_wire_txns[0].uuid, NULL, 0,
                                   NULL, NULL, false, 0, false, PK_PROTO_FAULT_INVALID_COUNTER);
        inject_rx_wire(wire, wlen);
        bool rej_wrong_route = port_slot_keys(0)->vcsec.valid && g_app.has_pending && s_wire_txns[0].active;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   NULL, s_wire_txns[0].uuid, NULL, 0,
                                   NULL, NULL, false, 0, false, PK_PROTO_FAULT_INVALID_COUNTER);
        inject_rx_wire(wire, wlen);
        bool rej_no_route = port_slot_keys(0)->vcsec.valid && g_app.has_pending && s_wire_txns[0].active;
        /* Wrong conn_gen directly into handle_rx_frame */
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid, NULL, 0,
                                   NULL, NULL, false, 0, false, PK_PROTO_FAULT_INVALID_COUNTER);
        handle_rx_frame(&g_app, 0, s_conn_gen + 99u, wire, wlen);
        bool rej_wrong_gen = port_slot_keys(0)->vcsec.valid && g_app.has_pending && s_wire_txns[0].active;
        check(rej_wrong_route && rej_no_route && rej_wrong_gen,
              "C4", "Fault with wrong route, missing route, or wrong conn_gen rejected cleanly");

        /* C5: Attributed outer unauthenticated fault -> resync hint only, NO write replay, NOT auth-rejected */
        int writes_before = g_gatt_write_calls;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid, NULL, 0,
                                   NULL, NULL, false, 0, false, PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES);
        inject_rx_wire(wire, wlen);
        check(!g_app.has_pending && g_gatt_write_calls == writes_before &&
              g_app.message != PK_MSG_AUTH_REJECTED && g_app.message != PK_MSG_CMD_REJECTED &&
              g_app.message == PK_MSG_LOCK_UNCONFIRMED,
              "C5", "Attributed outer fault never replays write, never claims auth-rejected, preserves unconfirmed");
    }

    /* ----------------------------------------------------------------------
     * SUITE D (§2D): DriveState Non-Finite / Conflicting Speed in Real port_ble.c
     * ---------------------------------------------------------------------- */
    {
        uint8_t cbuf[128], wire[512];
        CarServer_Response cr;

        /* D1: AES-GCM authenticated DriveState with P + speed=0 + speed_float=NaN -> REJECTED Park */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        memset(&cr, 0, sizeof(cr));
        cr.which_response_msg = CarServer_Response_vehicleData_tag;
        cr.response_msg.vehicleData.has_drive_state = true;
        cr.response_msg.vehicleData.drive_state.has_shift_state = true;
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_P_tag;
        cr.response_msg.vehicleData.drive_state.which_optional_speed = CarServer_DriveState_speed_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 0;
        cr.response_msg.vehicleData.drive_state.which_optional_speed_float = CarServer_DriveState_speed_float_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = NAN;
        pb_ostream_t os1 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os1, CarServer_Response_fields, &cr);
        size_t wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                          s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                          cbuf, os1.bytes_written,
                                          &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                          true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        check(!g_app.grant.parked && !g_app.grant.proof && g_app.message == PK_MSG_UNQUALIFIED,
              "D1", "Real port_ble.c rejects Park when speed=0 + speed_float=NaN (even under AES-GCM)");

        /* D2: Same NaN DriveState + ChargeState(SOC=82) during PK_PENDING_STATUS preserves SOC=82 */
        setup_connected_slot0(247u, true);
        pk_app_open_status(&g_app, PK_ORIGIN_LIST);
        cr.response_msg.vehicleData.has_charge_state = true;
        cr.response_msg.vehicleData.charge_state.which_optional_battery_level =
            CarServer_ChargeState_battery_level_tag;
        cr.response_msg.vehicleData.charge_state.optional_battery_level.battery_level = 82;
        pb_ostream_t os2 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os2, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, os2.bytes_written,
                                   &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        bool soc_preserved = false;
        for (unsigned i = 0; i < g_app.field_count; ++i) {
            if (strcmp(g_app.fields[i].name, "soc") == 0 &&
                g_app.fields[i].has_value && g_app.fields[i].value == 82) {
                soc_preserved = true;
            }
        }
        check(soc_preserved && !g_app.grant.parked,
              "D2", "Unrelated ChargeState SOC=82 preserved alongside NaN DriveState without granting Park");

        /* D3: Finite P + speed=0 + speed_float=0.0f under AES-GCM succeeds */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = 0.0f;
        pb_ostream_t os3 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os3, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, os3.bytes_written,
                                   &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.grant.parked && g_app.grant.proof,
              "D3", "Valid AES-GCM P + speed=0 + speed_float=0.0f qualifies Park normally");

        /* D4: authenticated D + NaN keeps the gear as motion and does not unlock. */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        memset(&cr, 0, sizeof(cr));
        cr.which_response_msg = CarServer_Response_vehicleData_tag;
        cr.response_msg.vehicleData.has_drive_state = true;
        cr.response_msg.vehicleData.drive_state.has_shift_state = true;
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_D_tag;
        cr.response_msg.vehicleData.drive_state.which_optional_speed = CarServer_DriveState_speed_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 0;
        cr.response_msg.vehicleData.drive_state.which_optional_speed_float = CarServer_DriveState_speed_float_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = NAN;
        pb_ostream_t os4 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os4, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, os4.bytes_written,
                                   &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        int gatt_before_unlock = g_gatt_write_calls;
        pk_intent_t unlock = pk_app_make_intent(&g_app, PK_ACTION_UNLOCK);
        bool unlock_accepted = pk_app_commit(&g_app, &unlock, g_app.now);
        check(pk_app_known_motion(&g_app) && !g_app.grant.parked &&
              !unlock_accepted && g_gatt_write_calls == gatt_before_unlock,
              "D4", "Authenticated D + NaN stays motion and does not write UNLOCK");

        /* D5: a finite non-zero speed survives a NaN sibling and is not Park. */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_P_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 5;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = NAN;
        pb_ostream_t os5 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os5, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, os5.bytes_written,
                                   &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        int gatt_before_flash = g_gatt_write_calls;
        pk_intent_t flash = pk_app_make_intent(&g_app, PK_ACTION_FLASH);
        bool flash_accepted = pk_app_commit(&g_app, &flash, g_app.now);
        check(pk_app_known_motion(&g_app) && !g_app.grant.parked &&
              !flash_accepted && g_gatt_write_calls == gatt_before_flash,
              "D5", "Finite 5 mph beside NaN is motion, not Park, and does not flash");

        /* D6: conflicting 0 and 5 are motion, never Park. */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 0;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = 5.0f;
        pb_ostream_t os6 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&os6, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, os6.bytes_written,
                                   &port_slot_keys(0)->infotainment, s_wire_txns[1].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        check(pk_app_known_motion(&g_app) && !g_app.grant.parked && !g_app.grant.proof,
              "D6", "Conflicting 0 and 5 mph is motion and does not grant Park");

        /* D7: a late authenticated wake ACK must not move the original deadline later. */
        setup_connected_slot0(247u, true);
        g_app.page = PK_PAGE_SETTINGS;
        g_app.vcsec_status_valid = true;
        g_app.vcsec_asleep = true;
        g_app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        g_app.caps.climate_on = PK_CAP_NO;
        (void)pk_app_activate_setting(&g_app, PK_SETTING_CLIMATE);
        pk_ms_t original_total = g_app.pending.deadline;
        g_now_ms = original_total - 1000u;
        g_app.now = g_now_ms;
        VCSEC_FromVCSECMessage wake_ack = VCSEC_FromVCSECMessage_init_zero;
        wake_ack.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
        wake_ack.sub_message.commandStatus.operationStatus =
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK;
        uint8_t vbuf_ack[64];
        pb_ostream_t vs_ack = pb_ostream_from_buffer(vbuf_ack, sizeof(vbuf_ack));
        pb_encode(&vs_ack, VCSEC_FromVCSECMessage_fields, &wake_ack);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   vbuf_ack, vs_ack.bytes_written,
                                   &port_slot_keys(0)->vcsec, s_wire_txns[0].req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.pending.kind == PK_PENDING_WAKE_THEN && g_app.pending.qphase == 1 &&
              g_app.pending.deadline <= original_total &&
              g_app.pending.deadline != g_app.now + PK_WAKE_SETTLE_MS,
              "D7", "Late wake ACK does not extend the original intent deadline");

        /* D8: opening status while the car is known asleep is not a wake. */
        setup_connected_slot0(247u, true);
        g_app.vcsec_status_valid = true;
        g_app.vcsec_asleep = true;
        g_app.vcsec_lock_state = PK_LOCK_READ_LOCKED;
        {
            int gatt_before_status = g_gatt_write_calls;
            pk_app_open_status(&g_app, PK_ORIGIN_LIST);
            check(g_app.pending.kind == PK_PENDING_STATUS &&
                  g_app.pending.action == PK_ACTION_STATUS_READ &&
                  !g_app.wake_spent && !g_app.wake_then_status &&
                  g_app.vcsec_status_valid &&
                  g_app.vcsec_lock_state == PK_LOCK_READ_LOCKED &&
                  g_gatt_write_calls > gatt_before_status,
                  "D8", "Known-asleep status open reads status and does not wake");
            original_total = g_app.pending.deadline;
            pk_app_open_status(&g_app, PK_ORIGIN_LIST);
            check(g_app.pending.kind == PK_PENDING_STATUS &&
                  g_app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE &&
                  !g_app.wake_spent &&
                  g_app.pending.deadline == original_total,
                  "D8", "Status reentry does not send WAKE_VEHICLE");
        }

        setup_connected_slot0(247u, true);
        g_app.page = PK_PAGE_SETTINGS;
        g_app.vcsec_status_valid = true;
        g_app.vcsec_asleep = true;
        g_app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        g_app.caps.climate_on = PK_CAP_NO;
        (void)pk_app_activate_setting(&g_app, PK_SETTING_CLIMATE);
        {
            int gatt_before_weak = g_gatt_write_calls;
            pk_ms_t weak_total = g_app.intent_deadline;
            memset(&wake_ack, 0, sizeof(wake_ack));
            wake_ack.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
            wake_ack.sub_message.commandStatus.operationStatus =
                VCSEC_OperationStatus_E_OPERATIONSTATUS_OK;
            vs_ack = pb_ostream_from_buffer(vbuf_ack, sizeof(vbuf_ack));
            pb_encode(&vs_ack, VCSEC_FromVCSECMessage_fields, &wake_ack);
            wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                       vbuf_ack, vs_ack.bytes_written,
                                       NULL, NULL, false, 0u, false, 0);
            inject_rx_wire(wire, wlen);
            check(!g_app.has_deferred && g_app.intent_deadline == 0 &&
                  g_app.message == PK_MSG_WAKE_FAIL &&
                  g_gatt_write_calls == gatt_before_weak,
                  "D8", "Weak command-wake ACK drops the gesture and does not resubmit");
            (void)weak_total;
        }

        setup_connected_slot0(247u, true);
        g_app.page = PK_PAGE_SETTINGS;
        g_app.vcsec_status_valid = true;
        g_app.vcsec_asleep = true;
        g_app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        g_app.caps.climate_on = PK_CAP_NO;
        (void)pk_app_activate_setting(&g_app, PK_SETTING_CLIMATE);
        {
            int gatt_before_err = g_gatt_write_calls;
            memset(&wake_ack, 0, sizeof(wake_ack));
            wake_ack.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
            wake_ack.sub_message.commandStatus.operationStatus =
                VCSEC_OperationStatus_E_OPERATIONSTATUS_ERROR;
            vs_ack = pb_ostream_from_buffer(vbuf_ack, sizeof(vbuf_ack));
            pb_encode(&vs_ack, VCSEC_FromVCSECMessage_fields, &wake_ack);
            wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                       vbuf_ack, vs_ack.bytes_written,
                                       &port_slot_keys(0)->vcsec, s_wire_txns[0].req_id,
                                       true, 1u, false, 0);
            inject_rx_wire(wire, wlen);
            check(!g_app.has_deferred && g_app.intent_deadline == 0 &&
                  g_app.message == PK_MSG_UNQUALIFIED &&
                  g_gatt_write_calls == gatt_before_err,
                  "D8", "Wake error drops the gesture and does not resubmit");
        }
    }

    /* ----------------------------------------------------------------------
     * SUITE E (§2E): Candidate RNG/Keygen Failure Blocks AddKey
     * ---------------------------------------------------------------------- */
    {
        const char *CAND_VIN = "5YJ30123456789ABC";

        /* E1: RNG failure during port_ble_start_candidate_pairing blocks pair entry */
        setup_connected_slot0(247u, false);
        simulate_ble_disconnect(); /* adding a vehicle requires disconnected or parked */
        memset(port_slot_keys(PK_VEHICLE_PAIR_CANDIDATE), 0, sizeof(pk_proto_key_slot_t));
        pk_app_enter_vin(&g_app);
        g_gatt_write_calls = 0;
        pk_test_set_rng_fail(true);
        port_ble_start_candidate_pairing(&g_app, CAND_VIN);
        pk_test_set_rng_fail(false);
        pk_proto_key_slot_t *cks = port_slot_keys(PK_VEHICLE_PAIR_CANDIDATE);
        check(g_app.page != PK_PAGE_PAIR && !cks->has_keypair && cks->pubkey[0] == 0 &&
              g_app.message == PK_MSG_PAIR_REJECTED && g_gatt_write_calls == 0,
              "E1", "RNG failure blocks candidate pairing entry with explicit error & 0 GATT writes");

        /* E2: Direct step 2 -> pk_app_pair_next with missing keypair blocks AddKey & stays on step 2 */
        (void)pk_app_pair_start(&g_app, CAND_VIN, true, true);
        (void)pk_app_pair_next(&g_app); /* step 1 -> step 2 */
        g_gatt_write_calls = 0;
        bool step2_ok = pk_app_pair_next(&g_app); /* step 2 -> AddKey blocked! */
        check(!step2_ok && g_app.pair_step == 2 && !g_app.enrollment_sent && !g_app.orphan &&
              g_app.message == PK_MSG_PAIR_REJECTED && g_gatt_write_calls == 0,
              "E2", "Missing candidate keypair blocks AddKey at step 2 (no zero pubkey, no quiet step 3 timeout)");

        /* E3: Restored RNG generates valid candidate keypair and enters pairing cleanly */
        pk_app_enter_vin(&g_app);
        port_ble_start_candidate_pairing(&g_app, CAND_VIN);
        check(g_app.page == PK_PAGE_PAIR && cks->has_keypair && cks->pubkey[0] == 0x04u,
              "E3", "Restored RNG generates valid P-256 candidate keypair & enters pairing");
    }

    /* ----------------------------------------------------------------------
     * SUITE L (§1): Supervisor Minimum Legacy Evidence x Purpose Table
     * ---------------------------------------------------------------------- */
    {
        uint8_t vbuf[128], cbuf[128], wire[512];
        VCSEC_FromVCSECMessage fm;
        CarServer_Response cr;

        /* L1: Legacy VCSEC VehicleStatus(LOCKED+ASLEEP) is display-only.
         * D07: the send gate is "not a known motion tendency". Legacy must
         * not change that gate, must not set vcsec_status_valid, and must
         * not move primary off LOCK. Connect must not set adapter_verified. */
        setup_connected_slot0(247u, false); /* unsigned probe path */
        bool unlock_before = pk_app_can_vcsec_unlock(&g_app);
        pk_action_t primary_before = pk_app_primary(&g_app);
        check(!g_app.caps.adapter_verified && primary_before == PK_ACTION_VCSEC_GET_STATUS_ONCE,
              "L1", "connect alone does not verify the adapter or select UNLOCK");
        pk_ble_frame_t probe = {
            .domain = PK_DOMAIN_VCSEC,
            .action = PK_ACTION_VCSEC_GET_STATUS_ONCE,
            .txn_id = 501,
            .scope = pk_app_scope(&g_app),
            .deadline = g_now_ms + 3000u,
        };
        (void)g_app.hal.ble->send_once(g_app.hal.ble_ctx, &probe);
        memset(&fm, 0, sizeof(fm));
        fm.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
        fm.sub_message.vehicleStatus.vehicleLockState = VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED;
        fm.sub_message.vehicleStatus.vehicleSleepStatus = VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_ASLEEP;
        pb_ostream_t vs1 = pb_ostream_from_buffer(vbuf, sizeof(vbuf));
        pb_encode(&vs1, VCSEC_FromVCSECMessage_fields, &fm);
        size_t wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                          s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                          vbuf, vs1.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.vcsec_observed_valid && g_app.vcsec_observed_asleep &&
              g_app.vcsec_observed_lock_state == PK_LOCK_READ_LOCKED &&
              !g_app.vcsec_status_valid && !g_app.caps.adapter_verified &&
              pk_app_can_vcsec_unlock(&g_app) == unlock_before &&
              pk_app_primary(&g_app) == primary_before &&
              pk_app_primary(&g_app) == PK_ACTION_VCSEC_GET_STATUS_ONCE,
              "L1", "Legacy VCSEC status updates display observation ONLY and does not change authorization");

        /* L2: Legacy VCSEC VehicleStatus(LOCKED) during PK_PENDING_LOCK_VERIFY NEVER confirms lock */
        setup_connected_slot0(247u, true);
        pk_intent_t lock_in = pk_app_make_intent(&g_app, PK_ACTION_LOCK);
        (void)pk_app_commit(&g_app, &lock_in, g_now_ms);
        (void)pk_app_command_result(&g_app, g_app.pending.id, PK_CMD_RESULT_ACCEPTED, true);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   vbuf, vs1.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.message != PK_MSG_LOCK_CONFIRMED && g_app.pending.kind == PK_PENDING_LOCK_VERIFY,
              "L2", "Legacy plaintext VCSEC status NEVER confirms lock verification");

        /* L3: Legacy Command Receipt -> honest PK_MSG_CMD_UNCONFIRMED, NEVER Accepted, NEVER mutates caps */
        setup_connected_slot0(247u, true);
        g_app.page = PK_PAGE_SETTINGS;
        g_app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        g_app.caps.climate_on = PK_CAP_NO;
        g_app.caps.climate_on = PK_CAP_NO;
        (void)pk_app_activate_setting(&g_app, PK_SETTING_CLIMATE);
        memset(&cr, 0, sizeof(cr));
        cr.has_actionStatus = true;
        cr.actionStatus.result = CarServer_OperationStatus_E_OPERATIONSTATUS_OK;
        pb_ostream_t cs1 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&cs1, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, cs1.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        check(!g_app.has_pending && !g_app.busy &&
              g_app.message == PK_MSG_CMD_UNCONFIRMED &&
              g_app.caps.climate_on == PK_CAP_NO,
              "L3", "Legacy command receipt surfaces honest PK_MSG_CMD_UNCONFIRMED & never mutates caps");

        /* L4: Legacy Infotainment status observation vs Park qualification separation */
        setup_connected_slot0(247u, true);
        g_app.caps.adapter_verified = false;
        pk_app_open_status(&g_app, PK_ORIGIN_LIST);
        memset(&cr, 0, sizeof(cr));
        cr.which_response_msg = CarServer_Response_vehicleData_tag;
        cr.response_msg.vehicleData.has_charge_state = true;
        cr.response_msg.vehicleData.charge_state.which_optional_battery_level =
            CarServer_ChargeState_battery_level_tag;
        cr.response_msg.vehicleData.charge_state.optional_battery_level.battery_level = 65;
        pb_ostream_t cs2 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&cs2, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, cs2.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        bool legacy_soc_shown = (g_app.field_count >= 1 &&
                                 strcmp(g_app.fields[0].name, "soc") == 0 &&
                                 g_app.fields[0].value == 65 &&
                                 !g_app.fields[0].clock_verified &&
                                 !g_app.caps.adapter_verified);
        /* Now try Park qualification via legacy plaintext -> MUST NOT grant Park */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, false, false);
        memset(&cr, 0, sizeof(cr));
        cr.which_response_msg = CarServer_Response_vehicleData_tag;
        cr.response_msg.vehicleData.has_drive_state = true;
        cr.response_msg.vehicleData.drive_state.has_shift_state = true;
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_P_tag;
        cr.response_msg.vehicleData.drive_state.which_optional_speed = CarServer_DriveState_speed_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 0;
        pb_ostream_t cs3 = pb_ostream_from_buffer(cbuf, sizeof(cbuf));
        pb_encode(&cs3, CarServer_Response_fields, &cr);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_INFOTAINMENT,
                                   s_wire_txns[1].routing_addr, s_wire_txns[1].uuid,
                                   cbuf, cs3.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        check(legacy_soc_shown && !g_app.grant.parked && !g_app.grant.proof,
              "L4", "Legacy status populates unverified display SOC=65; legacy DriveState NEVER grants Park");

        /* L5: Legacy bounded wake ACK: advances qualification read-only once, NEVER fires deferred write */
        setup_connected_slot0(247u, true);
        (void)pk_app_request_qualification(&g_app, true /* explicit user wake */, true);
        memset(&fm, 0, sizeof(fm));
        fm.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
        fm.sub_message.commandStatus.operationStatus = VCSEC_OperationStatus_E_OPERATIONSTATUS_OK;
        pb_ostream_t vs2 = pb_ostream_from_buffer(vbuf, sizeof(vbuf));
        pb_encode(&vs2, VCSEC_FromVCSECMessage_fields, &fm);
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   vbuf, vs2.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        bool qual_wake_advanced = (g_app.has_pending &&
                                   g_app.pending.kind == PK_PENDING_QUALIFICATION &&
                                   g_app.pending.qphase == PK_QPHASE_READ_WAIT &&
                                   g_app.pending.action == PK_ACTION_GET_QUALIFICATION);

        /* Now verify PK_PENDING_WAKE_THEN with deferred WRITE does NOT trigger write on legacy wake ACK */
        setup_connected_slot0(247u, true);
        g_app.page = PK_PAGE_SETTINGS;
        g_app.vcsec_status_valid = true;
        g_app.vcsec_asleep = true;
        g_app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        g_app.caps.climate_on = PK_CAP_NO;
        (void)pk_app_activate_setting(&g_app, PK_SETTING_CLIMATE);
        int writes_after_wake = g_gatt_write_calls;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   vbuf, vs2.bytes_written, NULL, NULL, false, 0, false, 0);
        inject_rx_wire(wire, wlen);
        bool deferred_write_blocked = (!g_app.has_pending && !g_app.has_deferred &&
                                       g_app.message == PK_MSG_WAKE_FAIL &&
                                       g_gatt_write_calls == writes_after_wake);
        check(qual_wake_advanced && deferred_write_blocked,
              "L5", "Legacy wake ACK advances read-only qualification once & NEVER triggers deferred write");
    }

    /* N12: after SoftAP is gone, a mismatched or missing VIN is an honest
     * retry prompt. The port must not invent a VIN. */
    pk_app_init(&g_app, PK_VEHICLE_NONE, true);
    g_app.ap = false;
    g_app.has_vin = false;
    port_ble_start_candidate_pairing(&g_app, "5YJ3E1EA1PF000001");
    check(g_app.message == PK_MSG_AP_EXPIRED && !g_app.has_vin,
          "N12", "closed hotspot does not invent a VIN");

    /* N14: Real AES-GCM passive VCSEC observation after wire txn is released
     * updates reported state via domain-session req_id ring; replay rejected;
     * background reconcile probe (txn_id==0) survives radio_cancel(0). */
    {
        uint8_t plain[64], wire[256];
        size_t plen, wlen;
        VCSEC_FromVCSECMessage vm;
        pb_ostream_t os;
        pk_proto_key_slot_t *ks;

        setup_connected_slot0(247u, true);
        ks = port_slot_keys(0);
        /* Record a signed req_id on VCSEC domain session, then release wire txn */
        pk_session_note_req_id(&s_session, 0, s_session.conn_gen,
                               PK_PROTO_DOMAIN_VEHICLE_SECURITY, s_wire_txns[0].req_id);
        s_wire_txns[0].active = false;
        g_app.vcsec_status_valid = false;
        g_app.vcsec_lock_state = PK_LOCK_READ_UNKNOWN;

        /* nanopb omits proto3 zero enums, so UNLOCKED(0) is written by hand. */
        {
            static const uint8_t unlocked_awake[] = {
                0x0A, 0x04, 0x10, 0x00, 0x18, 0x01
            };
            memcpy(plain, unlocked_awake, sizeof(unlocked_awake));
            plen = sizeof(unlocked_awake);
        }

        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   plain, plen, &ks->vcsec, s_wire_txns[0].req_id,
                                   true, 55u, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.vcsec_status_valid && g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED,
              "N14", "real AES-GCM passive VCSEC observation updates reported state after txn release");

        /* Replay of same counter 55 with different lock state MUST be rejected */
        memset(&vm, 0, sizeof(vm));
        vm.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
        vm.sub_message.vehicleStatus.vehicleSleepStatus =
            VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE;
        vm.sub_message.vehicleStatus.vehicleLockState = VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED;
        os = pb_ostream_from_buffer(plain, sizeof(plain));
        pb_encode(&os, VCSEC_FromVCSECMessage_fields, &vm);
        plen = os.bytes_written;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   plain, plen, &ks->vcsec, s_wire_txns[0].req_id,
                                   true, 55u, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED,
              "N14", "session rx_window rejects replayed passive observation counter");

        /* Background reconcile probe (txn_id==0) is not canceled by radio_cancel(0) */
        s_wire_txns[0].active = true;
        s_wire_txns[0].txn_id = 0u;
        s_wire_txns[0].action = PK_ACTION_VCSEC_GET_STATUS_ONCE;
        radio_cancel(NULL, 0u);
        check(s_wire_txns[0].active,
              "N14", "radio_cancel(0) preserves active background VCSEC reconcile probe");
    }

    /* N15: Consecutive signed requests on the same domain whose responses both
     * start at resp_counter==0 MUST both be accepted (per-req_id rx_window),
     * while replaying resp_counter==0 on the same req_id is still rejected. */
    {
        uint8_t plain[64], wire[256];
        uint8_t req_id_a[17], req_id_b[17];
        size_t plen, wlen;
        VCSEC_FromVCSECMessage vm;
        pb_ostream_t os;
        pk_proto_key_slot_t *ks;

        setup_connected_slot0(247u, true);
        ks = port_slot_keys(0);
        memset(req_id_a, 0xA1, sizeof(req_id_a));
        memset(req_id_b, 0xB2, sizeof(req_id_b));
        pk_session_note_req_id(&s_session, 0, s_session.conn_gen,
                               PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_a);
        pk_session_note_req_id(&s_session, 0, s_session.conn_gen,
                               PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_b);
        s_wire_txns[0].active = false;

        /* Request A reply with resp_counter == 0 -> LOCKED */
        memset(&vm, 0, sizeof(vm));
        vm.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
        vm.sub_message.vehicleStatus.vehicleLockState = VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED;
        vm.sub_message.vehicleStatus.vehicleSleepStatus = VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE;
        os = pb_ostream_from_buffer(plain, sizeof(plain));
        pb_encode(&os, VCSEC_FromVCSECMessage_fields, &vm);
        plen = os.bytes_written;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   plain, plen, &ks->vcsec, req_id_a,
                                   true, 0u, false, 0);
        inject_rx_wire(wire, wlen);
        bool got_a = (g_app.vcsec_status_valid && g_app.vcsec_lock_state == PK_LOCK_READ_LOCKED);

        /* Request B reply ALSO with resp_counter == 0 -> explicit UNLOCKED(0).
         * nanopb would omit that zero and the car would keep LOCKED. */
        {
            static const uint8_t unlocked_awake[] = {
                0x0A, 0x04, 0x10, 0x00, 0x18, 0x01
            };
            memcpy(plain, unlocked_awake, sizeof(unlocked_awake));
            plen = sizeof(unlocked_awake);
        }
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   plain, plen, &ks->vcsec, req_id_b,
                                   true, 0u, false, 0);
        inject_rx_wire(wire, wlen);
        bool got_b = (g_app.vcsec_status_valid && g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED);

        check(got_a && got_b,
              "N15", "consecutive requests with resp_counter==0 both accepted via per-req_id rx_window");
    }

    /* N16: a VehicleStatus that omits vehicleLockState must not become UNLOCKED. */
    {
        uint8_t plain[64], wire[256];
        uint8_t req_id[17];
        size_t plen, wlen;
        VCSEC_FromVCSECMessage vm;
        pb_ostream_t os;
        pk_proto_key_slot_t *ks;
        static const uint8_t omitted_asleep[] = { 0x0A, 0x02, 0x18, 0x02 };
        static const uint8_t explicit_unlocked[] = { 0x0A, 0x04, 0x10, 0x00, 0x18, 0x01 };
        bool shown_unlocked;

        setup_connected_slot0(247u, true);
        ks = port_slot_keys(0);
        memset(req_id, 0xC3, sizeof(req_id));
        pk_session_note_req_id(&s_session, 0, s_session.conn_gen,
                               PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id);
        s_wire_txns[0].active = false;

        memset(&vm, 0, sizeof(vm));
        vm.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
        vm.sub_message.vehicleStatus.vehicleLockState =
            VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED;
        vm.sub_message.vehicleStatus.vehicleSleepStatus =
            VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE;
        os = pb_ostream_from_buffer(plain, sizeof(plain));
        pb_encode(&os, VCSEC_FromVCSECMessage_fields, &vm);
        plen = os.bytes_written;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   plain, plen, &ks->vcsec, req_id,
                                   true, 0u, false, 0);
        inject_rx_wire(wire, wlen);

        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   omitted_asleep, sizeof(omitted_asleep), &ks->vcsec, req_id,
                                   true, 1u, false, 0);
        inject_rx_wire(wire, wlen);
        shown_unlocked = g_app.vcsec_status_valid &&
                         g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED;
        check(g_app.vcsec_status_valid && g_app.vcsec_lock_state == PK_LOCK_READ_LOCKED &&
              g_app.vcsec_asleep && !shown_unlocked,
              "N16", "omitted vehicleLockState keeps the confirmed lock");

        g_app.vcsec_status_valid = false;
        g_app.vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
        g_app.vcsec_asleep = false;
        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   omitted_asleep, sizeof(omitted_asleep), &ks->vcsec, req_id,
                                   true, 2u, false, 0);
        inject_rx_wire(wire, wlen);
        shown_unlocked = g_app.vcsec_status_valid &&
                         g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED;
        check(!g_app.vcsec_status_valid && g_app.vcsec_asleep && !shown_unlocked &&
              g_app.vcsec_lock_state != PK_LOCK_READ_UNLOCKED,
              "N16", "omitted vehicleLockState does not become UNLOCKED");

        wlen = build_routable_resp(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   s_wire_txns[0].routing_addr, s_wire_txns[0].uuid,
                                   explicit_unlocked, sizeof(explicit_unlocked),
                                   &ks->vcsec, req_id, true, 3u, false, 0);
        inject_rx_wire(wire, wlen);
        check(g_app.vcsec_status_valid && g_app.vcsec_lock_state == PK_LOCK_READ_UNLOCKED,
              "N16", "explicit vehicleLockState 0 is stored as unlocked");
    }

    printf("\nSUMMARY: %d PASS, %d FAIL (total %d)\n", g_pass, g_fail, g_pass + g_fail);
    return (g_fail == 0) ? 0 : 1;
}
