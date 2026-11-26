#include "port_internal.h"
#include "diag_log.h"
#include "pk_plan.h"
#include "pk_session.h"

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

#include <string.h>

static const char *TAG = "port_ble";

/* Active in-flight wire tracking for response correlation */
typedef struct {
    bool active;
    bool waiting_connect;
    bool waiting_session;
    bool addkey_transmitted;
    pk_ms_t last_approval_probe_ms;
    pk_id_t txn_id;
    pk_action_t action;
    pk_proto_domain_t domain;
    int slot;
    uint32_t conn_gen;
    uint32_t epoch_gen;
    pk_scope_t scope;
    pk_ms_t deadline;
    uint8_t inner[128];
    size_t inner_len;
    uint8_t routing_addr[16];
    uint8_t uuid[16];
    uint8_t req_id[17];
    pk_proto_rx_window_t rx_window;
    bool pair_vcsec_ok;
    bool pair_infotain_ok;
} active_wire_txn_t;

/* protocol.md invariants:
 *  - At most one in-flight request per domain is kept here (VCSEC + Infotainment),
 *    each with its own anti-replay window of official width 32 (D02).
 *  - VCSEC replies may omit request_uuid, so a reply is attributed by conn_gen,
 *    epoch_gen, routing address, and deadline inside pk_session_accept_rx(). */
static active_wire_txn_t s_wire_txns[2];
/* Session ownership lives in tesla_key; this port only feeds UUIDs and
 * bytes. */
static pk_session_ctx_t s_session;
static pk_ms_t s_conn_ready_ms = 0;
static SemaphoreHandle_t s_gatt_tx_sem = NULL;
/* S2 §2A: Bounded immutable callback-token pool matching NimBLE's procedure
 * lifecycle. Each ble_gattc_write_flat() returning 0 owns its slot until
 * NimBLE invokes on_gatt_write_cb() exactly once (ACK / ATT error / timeout /
 * disconnect) or nimble_port_stop() finishes in port_ble_deinit(). Slots are
 * NEVER overwritten while in_use==true; pool exhaustion fails explicitly. */
#define GATT_CB_TOK_POOL_CAP 8u
typedef struct {
    bool     in_use;
    uint32_t conn_gen;
    uint32_t write_seq;
} gatt_write_tok_t;
static pk_gatt_tx_t s_gatt_tx;
static gatt_write_tok_t s_gatt_tok_pool[GATT_CB_TOK_POOL_CAP];
static uint32_t s_gatt_tok_exhausted_drops = 0;
static int s_last_observed_vehicle = PK_VEHICLE_NONE;
static bool s_ble_params_applied = false;
static bool s_ble_last_fast = false;
static bool s_ble_rssi_valid = false;
static int8_t s_ble_rssi = -127;
static pk_ms_t s_ble_rssi_poll_ms = 0;

static active_wire_txn_t *wire_txn_for_domain(pk_proto_domain_t dom)
{
    return &s_wire_txns[(dom == PK_PROTO_DOMAIN_INFOTAINMENT) ? 1u : 0u];
}

static bool radio_now_ms(void *ctx, pk_ms_t *out)
{
    (void)ctx;
    if (!out) {
        return false;
    }
    *out = port_now_ms();
    return true;
}

/* ==========================================================================
 * 4. NimBLE Central + GATT Client Radio (0x0211 / 0x0212 / 0x0213)
 * ========================================================================== */

/* Tesla BLE Service & Characteristic UUIDs (little-endian for NimBLE BLE_UUID128_INIT)
 * Service: 00000211-b2d1-43f0-9b88-960cebf8b91e
 * TX Char: 00000212-b2d1-43f0-9b88-960cebf8b91e
 * RX Char: 00000213-b2d1-43f0-9b88-960cebf8b91e
 */
static const ble_uuid128_t k_uuid_svc = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00);

static const ble_uuid128_t k_uuid_tx = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x12, 0x02, 0x00, 0x00);

static const ble_uuid128_t k_uuid_rx = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00);

static const ble_uuid16_t k_uuid_cccd = BLE_UUID16_INIT(0x2902);

static portMUX_TYPE s_radio_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_ble_stack_ready;
static uint32_t s_conn_gen = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static uint16_t s_rx_val_handle;
static uint16_t s_cccd_handle;
static uint16_t s_svc_start_handle;
static pk_ms_t s_last_pair_scan_ms;
static uint16_t s_svc_end_handle;
static uint16_t s_peer_mtu = 23;
static int s_target_slot = PK_VEHICLE_NONE;
static int s_active_conn_slot = PK_VEHICLE_NONE;
static char s_target_ble_name[PK_PROTO_BLE_NAME_BUF];
static pk_rx_framer_t s_rx_framer;

static void radio_snapshot_ex(uint16_t *conn_out, uint16_t *tx_out, uint16_t *mtu_out,
                              int *slot_out, uint32_t *gen_out)
{
    portENTER_CRITICAL(&s_radio_mux);
    if (conn_out) *conn_out = s_conn_handle;
    if (tx_out) *tx_out = s_tx_val_handle;
    if (mtu_out) *mtu_out = s_peer_mtu;
    if (slot_out) *slot_out = s_active_conn_slot;
    if (gen_out) *gen_out = s_conn_gen;
    portEXIT_CRITICAL(&s_radio_mux);
}

static void radio_snapshot(uint16_t *conn_out, uint16_t *tx_out, uint16_t *mtu_out, int *slot_out)
{
    radio_snapshot_ex(conn_out, tx_out, mtu_out, slot_out, NULL);
}

static bool radio_get_mtu(void *ctx, uint16_t *out)
{
    uint16_t mtu = 23;

    (void)ctx;
    if (!out) {
        return false;
    }
    radio_snapshot(NULL, NULL, &mtu, NULL);
    if (mtu <= 23) {
        /* MTU exchange has not completed; the planner uses its protocol
         * fallback rather than treating an unnegotiated 23 as final. */
        return false;
    }
    *out = mtu;
    return true;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static pk_err_t radio_connect_once(void *ctx, int vehicle_slot, pk_ms_t scan_budget_ms);

static gatt_write_tok_t *gatt_tok_alloc_locked(uint32_t conn_gen, uint32_t write_seq)
{
    for (size_t i = 0; i < GATT_CB_TOK_POOL_CAP; ++i) {
        if (!s_gatt_tok_pool[i].in_use) {
            s_gatt_tok_pool[i].in_use = true;
            s_gatt_tok_pool[i].conn_gen = conn_gen;
            s_gatt_tok_pool[i].write_seq = write_seq;
            return &s_gatt_tok_pool[i];
        }
    }
    s_gatt_tok_exhausted_drops++;
    return NULL;
}

static int on_gatt_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    gatt_write_tok_t *tok = (gatt_write_tok_t *)arg;
    int status = error ? error->status : 0;
    pk_err_t qerr = PK_OK;
    uint32_t cb_gen = 0u;
    uint32_t cb_seq = 0u;
    bool valid_tok = false;
    (void)conn_handle;
    (void)attr;
    if (tok) {
        portENTER_CRITICAL(&s_radio_mux);
        if (tok >= &s_gatt_tok_pool[0] &&
            tok < &s_gatt_tok_pool[GATT_CB_TOK_POOL_CAP] &&
            tok->in_use) {
            cb_gen = tok->conn_gen;
            cb_seq = tok->write_seq;
            /* Procedure terminated in NimBLE: release slot only here. */
            tok->in_use = false;
            valid_tok = true;
            qerr = pk_gatt_tx_enqueue_cb(&s_gatt_tx, cb_gen, cb_seq, status);
        }
        portEXIT_CRITICAL(&s_radio_mux);
        if (valid_tok && qerr == PK_ERR_BUSY) {
            ESP_LOGW(TAG, "gatt tx cb queue overflow (gen=%u seq=%u)",
                     (unsigned)cb_gen, (unsigned)cb_seq);
        }
    }
    if (s_gatt_tx_sem) {
        xSemaphoreGive(s_gatt_tx_sem);
    }
    return 0;
}

static pk_tx_outcome_t gatt_write_framed(const uint8_t *msg, size_t msg_len, pk_ms_t deadline_ms)
{
    static uint8_t s_tx_framed[PK_PROTO_MAX_FRAME_LEN + 2];
    uint16_t conn_h;
    uint16_t tx_h;
    uint32_t conn_gen;
    uint32_t write_seq;
    size_t framed_len;
    size_t offset = 0;
    pk_ms_t now = port_now_ms();
    pk_tx_outcome_t outcome = PK_TX_NOT_SENT;
    bool started;

    /* S2 §2B: Without a completion semaphore/channel, NEVER send or degrade to
     * PK_TX_DELIVERED; fail closed with PK_TX_NOT_SENT. */
    if (!s_gatt_tx_sem) {
        ESP_LOGE(TAG, "gatt tx completion channel unavailable; fail-closed");
        return PK_TX_NOT_SENT;
    }

    framed_len = pk_proto_frame_wrap(msg, msg_len, s_tx_framed, sizeof(s_tx_framed));
    if (framed_len == 0) {
        return PK_TX_NOT_SENT;
    }

    portENTER_CRITICAL(&s_radio_mux);
    conn_h = s_conn_handle;
    tx_h = s_tx_val_handle;
    conn_gen = s_conn_gen;
    if (conn_h != BLE_HS_CONN_HANDLE_NONE && tx_h != 0) {
        pk_gatt_tx_set_mtu(&s_gatt_tx, s_peer_mtu);
        started = pk_gatt_tx_begin(&s_gatt_tx, framed_len, now, deadline_ms);
        write_seq = s_gatt_tx.write_seq;
    } else {
        started = false;
        write_seq = 0;
    }
    portEXIT_CRITICAL(&s_radio_mux);

    if (!started) {
        return PK_TX_NOT_SENT;
    }
    (void)xSemaphoreTake(s_gatt_tx_sem, 0);

    while (offset < framed_len) {
        size_t chunk;
        pk_ms_t rem_ms;
        bool link_up;
        int rc;
        pk_err_t step_err;
        size_t stepped = 0;
        gatt_write_tok_t *tok = NULL;

        now = port_now_ms();
        portENTER_CRITICAL(&s_radio_mux);
        link_up = (s_conn_handle == conn_h && s_conn_gen == conn_gen && s_tx_val_handle == tx_h);
        rem_ms = pk_gatt_tx_remaining_ms(&s_gatt_tx, now);
        chunk = pk_gatt_tx_chunk_size(&s_gatt_tx);
        if (chunk > framed_len - offset) {
            chunk = framed_len - offset;
        }
        if (!link_up || rem_ms == 0u) {
            (void)pk_gatt_tx_step(&s_gatt_tx, now, link_up, 0, NULL);
            outcome = s_gatt_tx.last_outcome;
            portEXIT_CRITICAL(&s_radio_mux);
            return outcome;
        }
        tok = gatt_tok_alloc_locked(conn_gen, write_seq);
        if (!tok) {
            /* Pool exhausted by unresolved NimBLE procedures: fail explicitly,
             * never overwrite an active procedure's token. */
            s_gatt_tx.in_flight = false;
            s_gatt_tx.last_outcome = (s_gatt_tx.bytes_sent > 0u)
                                         ? PK_TX_SENT_UNKNOWN
                                         : PK_TX_NOT_SENT;
            outcome = s_gatt_tx.last_outcome;
            portEXIT_CRITICAL(&s_radio_mux);
            ESP_LOGW(TAG, "gatt tx token pool exhausted (drops=%u)",
                     (unsigned)s_gatt_tok_exhausted_drops);
            return outcome;
        }
        portEXIT_CRITICAL(&s_radio_mux);

        /* protocol.md requires "write with response". Always attach immutable
         * callback token; never fall back to write-without-response. */
        rc = ble_gattc_write_flat(conn_h, tx_h,
                                  s_tx_framed + offset, (uint16_t)chunk,
                                  on_gatt_write_cb, tok);

        portENTER_CRITICAL(&s_radio_mux);
        if (rc != 0) {
            /* NimBLE did not allocate a procedure and will never invoke cb. */
            tok->in_use = false;
        }
        link_up = (s_conn_handle == conn_h && s_conn_gen == conn_gen && s_tx_val_handle == tx_h);
        step_err = pk_gatt_tx_step(&s_gatt_tx, port_now_ms(), link_up, rc, &stepped);
        outcome = s_gatt_tx.last_outcome;
        portEXIT_CRITICAL(&s_radio_mux);

        if (step_err != PK_OK) {
            ESP_LOGW(TAG, "write-with-response failed rc=%d err=%d off=%u",
                     rc, (int)step_err, (unsigned)offset);
            return outcome;
        }

        {
            bool chunk_acked = false;
            while (!chunk_acked) {
                pk_ms_t wait_now = port_now_ms();
                pk_ms_t wait_ms;
                bool matched = false;
                pk_err_t cb_err;
                bool still_up;
                uint8_t queued;

                portENTER_CRITICAL(&s_radio_mux);
                still_up = (s_conn_handle == conn_h && s_conn_gen == conn_gen);
                cb_err = pk_gatt_tx_poll_cb(&s_gatt_tx, wait_now, &matched);
                queued = s_gatt_tx.cb_count;
                wait_ms = pk_gatt_tx_remaining_ms(&s_gatt_tx, wait_now);
                outcome = s_gatt_tx.last_outcome;
                portEXIT_CRITICAL(&s_radio_mux);

                if (matched) {
                    if (cb_err == PK_OK) {
                        chunk_acked = true;
                        break;
                    }
                    return outcome;
                }
                if (cb_err == PK_ERR_EXPIRED || !still_up || wait_ms == 0u) {
                    portENTER_CRITICAL(&s_radio_mux);
                    if (s_gatt_tx.in_flight && s_gatt_tx.conn_gen == conn_gen &&
                        s_gatt_tx.write_seq == write_seq) {
                        s_gatt_tx.in_flight = false;
                        s_gatt_tx.last_outcome = (s_gatt_tx.bytes_sent > 0u)
                                                     ? PK_TX_SENT_UNKNOWN
                                                     : PK_TX_NOT_SENT;
                    }
                    outcome = s_gatt_tx.last_outcome;
                    portEXIT_CRITICAL(&s_radio_mux);
                    return outcome;
                }
                if (queued > 0u) {
                    continue;
                }
                if (wait_ms > 1000u) {
                    wait_ms = 1000u;
                }
                (void)xSemaphoreTake(s_gatt_tx_sem, pdMS_TO_TICKS(wait_ms));
            }
        }
        offset += stepped;
    }

    portENTER_CRITICAL(&s_radio_mux);
    outcome = s_gatt_tx.last_outcome;
    portEXIT_CRITICAL(&s_radio_mux);
    return outcome;
}

static void send_domain_session_request(int slot, pk_proto_domain_t domain)
{
    pk_proto_key_slot_t *ks = port_slot_keys(slot);
    uint8_t addr[16];
    uint8_t uuid[16];
    uint8_t out[160];
    size_t len;
    uint32_t conn_gen = 0;
    pk_ms_t now = port_now_ms();

    if (!ks || !ks->has_keypair || ks->pubkey[0] != 0x04u) {
        return;
    }
    if (!pk_proto_random_bytes(addr, sizeof(addr)) ||
        !pk_proto_random_bytes(uuid, sizeof(uuid))) {
        pk_proto_zeroize(addr, sizeof(addr));
        pk_proto_zeroize(uuid, sizeof(uuid));
        ESP_LOGW(TAG, "RNG unavailable for SessionInfo request; fail-closed");
        return;
    }
    radio_snapshot_ex(NULL, NULL, NULL, NULL, &conn_gen);
    pk_proto_route_t route = { domain, addr, uuid };
    len = pk_proto_build_session_info_request(&route, ks->pubkey, out, sizeof(out));
    if (len > 0 && gatt_write_framed(out, len, now + 2000u) == PK_TX_DELIVERED) {
        /* Record attribution only after the handshake actually left the
         * radio; a failed write must not suppress the next retry. */
        pk_session_note_handshake_ex(&s_session, slot, conn_gen, domain, addr, uuid, now);
    }
}

static void request_session_if_idle(int slot, pk_proto_domain_t domain)
{
    pk_ms_t now = port_now_ms();
    pk_proto_key_slot_t *ks = port_slot_keys(slot);
    pk_proto_session_t *sess = NULL;

    /* protocol.md "Caching session state": if a domain session is still valid
     * after a BLE reconnect, reuse it and skip the SessionInfo handshake. A
     * stale session (vehicle rebooted) is recovered from the auth fault path. */
    if (!ks || !ks->has_keypair || ks->pubkey[0] != 0x04u) {
        return;
    }
    sess = (domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY)
               ? &ks->vcsec
               : &ks->infotainment;
    if (sess && sess->valid) {
        return;
    }
    if (pk_session_handshake_recent(&s_session, slot, domain, now, 1500u)) {
        return;
    }
    send_domain_session_request(slot, domain);
}

static void post_ble_connected(int slot, uint32_t conn_gen)
{
    port_event_t ev;
    if (slot == PK_VEHICLE_NONE) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = PORT_EV_BLE_CONNECTED;
    ev.slot = slot;
    ev.conn_gen = conn_gen;
    (void)port_post_event(&ev);
}

static int on_mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t mtu, void *arg)
{
    int slot = PK_VEHICLE_NONE;
    uint32_t conn_gen = 0;
    bool current_conn;
    (void)arg;

    portENTER_CRITICAL(&s_radio_mux);
    current_conn = (s_conn_handle == conn_handle && conn_handle != BLE_HS_CONN_HANDLE_NONE);
    if (current_conn) {
        if (error && error->status == 0 && mtu > 23u) {
            s_peer_mtu = mtu;
            pk_gatt_tx_set_mtu(&s_gatt_tx, mtu);
        } else {
            s_peer_mtu = 23u;
            pk_gatt_tx_set_mtu(&s_gatt_tx, 23u);
        }
        slot = s_active_conn_slot;
        conn_gen = s_conn_gen;
    }
    portEXIT_CRITICAL(&s_radio_mux);

    if (!current_conn) {
        return 0;
    }
    if (error && error->status == 0 && mtu > 23u) {
        diag_event(DIAG_MTU, 0, 0, (int)mtu, 0);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed (status=%d, mtu=%u); fallback to DefaultMTU-3 (20B)",
                 error ? error->status : -1, (unsigned)mtu);
    }
    post_ble_connected(slot, conn_gen);
    return 0;
}

static int on_cccd_written(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    int slot;
    uint32_t conn_gen;
    (void)attr;
    (void)arg;

    radio_snapshot_ex(NULL, NULL, NULL, &slot, &conn_gen);
    if (error && error->status == 0 && slot != PK_VEHICLE_NONE) {
        int rc = ble_gattc_exchange_mtu(conn_handle, on_mtu_exchanged, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed (rc=%d); fallback to DefaultMTU-3 (20B)",
                     rc);
            portENTER_CRITICAL(&s_radio_mux);
            if (s_conn_handle == conn_handle) {
                s_peer_mtu = 23u;
                pk_gatt_tx_set_mtu(&s_gatt_tx, 23u);
            }
            portEXIT_CRITICAL(&s_radio_mux);
            post_ble_connected(slot, conn_gen);
        }
    }
    return 0;
}

static int on_dsc_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (!error) {
        return 0;
    }
    if (error->status == 0 && dsc) {
        if (s_cccd_handle == 0 && ble_uuid_cmp(&dsc->uuid.u, &k_uuid_cccd.u) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_cccd_handle != 0) {
            uint8_t val[2] = { 0x01, 0x00 }; /* Enable notifications */
            (void)ble_gattc_write_flat(conn_handle, s_cccd_handle, val, sizeof(val),
                                       on_cccd_written, NULL);
        } else {
            ESP_LOGW(TAG, "CCCD not found, terminating conn=%u", (unsigned)conn_handle);
            (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    ESP_LOGW(TAG, "dsc discovery error=%d, terminating conn=%u",
             error->status, (unsigned)conn_handle);
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_chr_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!error) {
        return 0;
    }
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &k_uuid_tx.u) == 0) {
            s_tx_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &k_uuid_rx.u) == 0) {
            s_rx_val_handle = chr->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_rx_val_handle != 0 && s_tx_val_handle != 0 && s_svc_end_handle != 0) {
            s_cccd_handle = 0;
            (void)ble_gattc_disc_all_dscs(conn_handle, s_rx_val_handle, s_svc_end_handle,
                                          on_dsc_discovered, NULL);
        } else {
            ESP_LOGW(TAG, "TX/RX chr not found, terminating conn=%u", (unsigned)conn_handle);
            (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    ESP_LOGW(TAG, "chr discovery error=%d, terminating conn=%u",
             error->status, (unsigned)conn_handle);
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_svc_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (!error) {
        return 0;
    }
    if (error->status == 0 && service) {
        if (s_svc_start_handle == 0 &&
            service->start_handle != 0 && service->start_handle != 0xFFFF &&
            service->start_handle <= service->end_handle) {
            s_svc_start_handle = service->start_handle;
            s_svc_end_handle = service->end_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start_handle != 0) {
            (void)ble_gattc_disc_all_chrs(conn_handle, s_svc_start_handle, s_svc_end_handle,
                                          on_chr_discovered, NULL);
        } else {
            ESP_LOGW(TAG, "Tesla BLE svc not found, terminating conn=%u", (unsigned)conn_handle);
            (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    ESP_LOGW(TAG, "svc discovery error=%d, terminating conn=%u",
             error->status, (unsigned)conn_handle);
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static bool adv_matches_target(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    char target_name[PK_PROTO_BLE_NAME_BUF];

    portENTER_CRITICAL(&s_radio_mux);
    memcpy(target_name, s_target_ble_name, sizeof(target_name));
    portEXIT_CRITICAL(&s_radio_mux);
    if (target_name[0] == '\0') {
        return false;
    }
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }
    if (fields.name && fields.name_len == PK_PROTO_BLE_NAME_LEN &&
        memcmp(fields.name, target_name, PK_PROTO_BLE_NAME_LEN) == 0) {
        return true;
    }
    return false;
}

static void handle_ble_notify_rx(struct ble_gap_event *event)
{
    uint16_t om_len = OS_MBUF_PKTLEN(event->notify_rx.om);
    uint8_t chunk[512];
    size_t off = 0;
    pk_ms_t now = port_now_ms();
    int slot;
    uint32_t conn_gen;

    if (om_len == 0 || om_len > sizeof(chunk)) {
        return;
    }
    (void)ble_hs_mbuf_to_flat(event->notify_rx.om, chunk, om_len, NULL);
    radio_snapshot_ex(NULL, NULL, NULL, &slot, &conn_gen);

    /* D05 bounded streaming reassembly: emit every coalesced frame in a single
     * notification and keep any trailing prefix for the next notification. */
    while (off < om_len) {
        size_t consumed = 0;
        int rc = pk_rx_framer_feed_ex(&s_rx_framer, chunk + off,
                                      (size_t)om_len - off, now, &consumed);
        if (rc == 1) {
            port_event_t ev;
            int buf_idx = port_rx_buf_alloc_copy(s_rx_framer.buf, s_rx_framer.expected_len);
            if (buf_idx >= 0) {
                memset(&ev, 0, sizeof(ev));
                ev.type = PORT_EV_BLE_RX_FRAME;
                ev.slot = slot;
                ev.conn_gen = conn_gen;
                ev.len = (uint16_t)s_rx_framer.expected_len;
                ev.u.rx_buf_idx = (int8_t)buf_idx;
                if (!port_post_event(&ev)) {
                    port_rx_buf_release(buf_idx);
                }
            }
        }
        /* Illegal length already discarded this chunk. Do not scan the tail. */
        if (consumed == 0 || rc < 0) {
            break;
        }
        off += consumed;
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_matches_target(&event->disc)) {
            if (event->disc.rssi >= -80 &&
                (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND ||
                 event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND)) {
                port_event_t ev;
                ESP_LOGW(TAG, "target adv (%s) NONCONN rssi=%d -> slots full",
                         s_target_ble_name, (int)event->disc.rssi);
                (void)ble_gap_disc_cancel();
                memset(&ev, 0, sizeof(ev));
                ev.type = PORT_EV_BLE_SLOTS_FULL;
                ev.slot = s_target_slot;
                (void)port_post_event(&ev);
                return 0;
            }
            uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
            ESP_LOGI(TAG, "matched target adv (%s), connecting", s_target_ble_name);
            diag_event(DIAG_SCAN, 0, 0, 1, (int)event->disc.rssi);
            (void)ble_gap_disc_cancel();
            (void)ble_hs_id_infer_auto(0, &own_addr_type);
            (void)ble_gap_connect(own_addr_type, &event->disc.addr, 3000, NULL,
                                  ble_gap_event_cb, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        diag_event(DIAG_CONN, 0, 0, event->connect.status, 0);
        if (event->connect.status == 0) {
            portENTER_CRITICAL(&s_radio_mux);
            s_conn_gen++;
            if (s_conn_gen == 0u) {
                s_conn_gen = 1u;
            }
            s_conn_handle = event->connect.conn_handle;
            s_active_conn_slot = s_target_slot;
            s_tx_val_handle = 0;
            s_rx_val_handle = 0;
            s_cccd_handle = 0;
            s_svc_start_handle = 0;
            s_svc_end_handle = 0;
            s_peer_mtu = 23;
            s_ble_params_applied = false;
            pk_gatt_tx_init(&s_gatt_tx, s_conn_gen, 23u);
            portEXIT_CRITICAL(&s_radio_mux);
            pk_rx_framer_init(&s_rx_framer);
            (void)ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &k_uuid_svc.u,
                                             on_svc_discovered, NULL);
        } else {
            ESP_LOGW(TAG, "ble_gap_connect failed: %d", event->connect.status);
            portENTER_CRITICAL(&s_radio_mux);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_active_conn_slot = PK_VEHICLE_NONE;
            s_ble_params_applied = false;
            portEXIT_CRITICAL(&s_radio_mux);
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "ble_gap_disc complete: reason=%d", event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.value > 23) {
            portENTER_CRITICAL(&s_radio_mux);
            s_peer_mtu = event->mtu.value;
            pk_gatt_tx_set_mtu(&s_gatt_tx, event->mtu.value);
            portEXIT_CRITICAL(&s_radio_mux);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        port_event_t ev;
        int prev_slot;
        uint32_t disc_gen;
        ESP_LOGW(TAG, "ble disconnected: reason=0x%02x", event->disconnect.reason);
        diag_event(DIAG_DISC, 0, 0, event->disconnect.reason, 0);
        portENTER_CRITICAL(&s_radio_mux);
        prev_slot = s_active_conn_slot;
        s_conn_gen++;
        if (s_conn_gen == 0u) {
            s_conn_gen = 1u;
        }
        disc_gen = s_conn_gen;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_active_conn_slot = PK_VEHICLE_NONE;
        s_ble_params_applied = false;
        s_tx_val_handle = 0;
        s_rx_val_handle = 0;
        pk_gatt_tx_on_disconnect(&s_gatt_tx, disc_gen);
        portEXIT_CRITICAL(&s_radio_mux);
        s_conn_ready_ms = 0;
        if (s_gatt_tx_sem) {
            xSemaphoreGive(s_gatt_tx_sem);
        }
        memset(&ev, 0, sizeof(ev));
        ev.type = PORT_EV_BLE_DISCONNECTED;
        ev.slot = prev_slot;
        ev.conn_gen = disc_gen;
        (void)port_post_event(&ev);
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_ble_notify_rx(event);
        return 0;

    default:
        return 0;
    }
}

static bool radio_has_session(void *ctx, int vehicle_slot, pk_domain_t domain)
{
    pk_proto_key_slot_t *ks;
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;
    (void)ctx;
    (void)domain;

    if (vehicle_slot == PK_VEHICLE_PAIR_CANDIDATE) {
        return true;
    }
    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (conn_h == BLE_HS_CONN_HANDLE_NONE || tx_h == 0 || active_slot != vehicle_slot) {
        return false;
    }
    ks = port_slot_keys(vehicle_slot);
    return (ks != NULL && ks->has_keypair);
}

static pk_err_t tx_outcome_to_err(pk_tx_outcome_t outcome)
{
    pk_app_t *app = port_bound_app();
    if (app) {
        app->last_tx_outcome = outcome;
    }
    if (outcome == PK_TX_DELIVERED) {
        return PK_OK;
    }
    if (outcome == PK_TX_SENT_UNKNOWN) {
        return PK_ERR_UNKNOWN;
    }
    return PK_ERR_NOT_WIRED;
}

static pk_tx_outcome_t send_addkey_direct(const active_wire_txn_t *txn)
{
    if (!txn || txn->inner_len == 0) {
        return PK_TX_NOT_SENT;
    }
    return gatt_write_framed(txn->inner, txn->inner_len, txn->deadline);
}

static pk_err_t start_addkey_txn(active_wire_txn_t *txn, const pk_proto_key_slot_t *ks,
                                 bool gatt_ready, uint32_t conn_gen)
{
    uint8_t inner[128];
    size_t inner_len;
    pk_tx_outcome_t outcome;

    /* S2 §2E: Never encode or transmit a zero/uninitialized candidate pubkey. */
    if (!ks || !ks->has_keypair || ks->pubkey[0] != 0x04u) {
        ESP_LOGE(TAG, "addkey blocked: candidate keypair missing/uninitialized");
        txn->active = false;
        return PK_ERR_INVALID;
    }
    inner_len = pk_proto_encode_vcsec_add_key(
        ks->pubkey, pk_proto_pair_key_role(), PK_PROTO_KEY_FORM_CLOUD_KEY,
        inner, sizeof(inner));
    if (inner_len == 0) {
        txn->active = false;
        return PK_ERR_INVALID;
    }
    memcpy(txn->inner, inner, inner_len);
    txn->inner_len = inner_len;
    if (!gatt_ready) {
        uint16_t conn_h;
        txn->waiting_connect = true;
        radio_snapshot(&conn_h, NULL, NULL, NULL);
        if (conn_h == BLE_HS_CONN_HANDLE_NONE && !ble_gap_disc_active()) {
            (void)radio_connect_once(NULL, PK_VEHICLE_PAIR_CANDIDATE, PK_PAIR_APPROVAL_MS);
        }
        ESP_LOGI(TAG, "addkey queued waiting_connect (conn=%u, disc=%d)",
                 (unsigned)conn_h, ble_gap_disc_active() ? 1 : 0);
        return PK_OK;
    }
    outcome = send_addkey_direct(txn);
    if (outcome != PK_TX_DELIVERED) {
        txn->active = false;
        return tx_outcome_to_err(outcome);
    }
    ESP_LOGI(TAG, "addkey sent (%u bytes ToVCSECMessage)", (unsigned)inner_len);
    txn->conn_gen = conn_gen;
    txn->addkey_transmitted = true;
    txn->last_approval_probe_ms = port_now_ms();
    pk_session_note_request_ex(&s_session, txn->slot, conn_gen, 0u, txn->domain,
                               txn->routing_addr, txn->uuid,
                               false, txn->last_approval_probe_ms);
    return tx_outcome_to_err(PK_TX_DELIVERED);
}

static pk_err_t start_pair_verify_txn(active_wire_txn_t *txn, int slot, bool gatt_ready)
{
    txn->pair_vcsec_ok = false;
    txn->pair_infotain_ok = false;
    txn->last_approval_probe_ms = port_now_ms();
    if (!gatt_ready) {
        txn->waiting_connect = true;
        return PK_OK;
    }
    send_domain_session_request(slot, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
    return PK_OK;
}

static pk_err_t radio_write_once(void *ctx, const pk_ble_frame_t *frame)
{
    const char *vin;
    pk_proto_key_slot_t *ks;
    uint8_t inner[128];
    size_t inner_len;
    static uint8_t s_tx_wire[512];
    size_t wire_len = 0;
    uint8_t nonce[12];
    pk_proto_domain_t pdom;
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;
    uint32_t conn_gen = 0;
    bool gatt_ready;
    bool unsigned_probe;
    active_wire_txn_t *txn;

    (void)ctx;
    if (!frame) {
        return PK_ERR_NOT_WIRED;
    }
    radio_snapshot_ex(&conn_h, &tx_h, NULL, &active_slot, &conn_gen);
    gatt_ready = (conn_h != BLE_HS_CONN_HANDLE_NONE && tx_h != 0 &&
                  active_slot == frame->scope.vehicle);
    if (!gatt_ready && frame->scope.vehicle != PK_VEHICLE_PAIR_CANDIDATE) {
        return PK_ERR_NOT_WIRED;
    }
    vin = port_slot_vin(frame->scope.vehicle);
    ks = port_slot_keys(frame->scope.vehicle);
    if (!vin || !ks) {
        return PK_ERR_NOT_WIRED;
    }
    if (frame->action == PK_ACTION_PAIR_VERIFY_INFOTAINMENT) {
        /* Dispatched serially after VCSEC SessionInfo succeeds in handle_rx_frame */
        return PK_OK;
    }

    pdom = (frame->domain == PK_DOMAIN_VCSEC)
               ? PK_PROTO_DOMAIN_VEHICLE_SECURITY
               : PK_PROTO_DOMAIN_INFOTAINMENT;
    txn = wire_txn_for_domain(pdom);
    if (txn->active && txn->txn_id != frame->txn_id) {
        return PK_ERR_BUSY; /* never overwrite/queue behind another wire request */
    }
    memset(txn, 0, sizeof(*txn));
    txn->active = true;
    txn->txn_id = frame->txn_id;
    txn->action = frame->action;
    txn->domain = pdom;
    txn->slot = frame->scope.vehicle;
    txn->conn_gen = conn_gen;
    txn->scope = frame->scope;
    txn->deadline = frame->deadline;
    pk_proto_rx_window_init(&txn->rx_window);
    if (!pk_proto_random_bytes(txn->routing_addr, sizeof(txn->routing_addr)) ||
        !pk_proto_random_bytes(txn->uuid, sizeof(txn->uuid)) ||
        !pk_proto_random_bytes(nonce, sizeof(nonce))) {
        pk_proto_zeroize(nonce, sizeof(nonce));
        txn->active = false;
        ESP_LOGW(TAG, "RNG unavailable for radio_write_once; fail-closed");
        (void)tx_outcome_to_err(PK_TX_NOT_SENT);
        return PK_ERR_INVALID;
    }

    if (frame->action == PK_ACTION_ADDKEY) {
        pk_proto_zeroize(nonce, sizeof(nonce));
        return start_addkey_txn(txn, ks, gatt_ready, conn_gen);
    }
    if (frame->action == PK_ACTION_PAIR_VERIFY_VCSEC) {
        pk_proto_zeroize(nonce, sizeof(nonce));
        return start_pair_verify_txn(txn, frame->scope.vehicle, gatt_ready);
    }

    if (frame->payload_len > 0 && frame->payload_len <= sizeof(inner)) {
        memcpy(inner, frame->payload, frame->payload_len);
        inner_len = frame->payload_len;
    } else {
        inner_len = pk_proto_encode_action_payload(
            frame->action, port_bound_app() ? &port_bound_app()->draft : NULL,
            ks->pubkey, inner, sizeof(inner));
    }
    if (inner_len == 0) {
        pk_proto_zeroize(nonce, sizeof(nonce));
        txn->active = false;
        return PK_ERR_INVALID;
    }
    memcpy(txn->inner, inner, inner_len);
    txn->inner_len = inner_len;

    unsigned_probe = (frame->action == PK_ACTION_VCSEC_GET_STATUS_ONCE &&
                      !ks->vcsec.valid);
    if (unsigned_probe) {
        pk_proto_route_t route = { pdom, txn->routing_addr, txn->uuid };
        pk_proto_zeroize(nonce, sizeof(nonce));
        wire_len = pk_proto_build_unsigned_routable(
            &route, inner, inner_len, s_tx_wire, sizeof(s_tx_wire));
    } else {
        pk_proto_session_t *sess = (pdom == PK_PROTO_DOMAIN_VEHICLE_SECURITY)
                                       ? &ks->vcsec
                                       : &ks->infotainment;
        pk_proto_signed_req_t req = {
            .route         = { pdom, txn->routing_addr, txn->uuid },
            .vin           = vin,
            .client_pubkey = ks->pubkey,
            .inner_payload = inner,
            .inner_len     = inner_len,
            .nonce         = nonce,
            .now_ms        = port_now_ms(),
            .ttl_seconds   = 5u,
        };
        if (!sess->valid) {
            /* Domain session not yet established: request SessionInfo once.
             * Queued is not delivered. A stale DELIVERED must not survive. */
            pk_proto_zeroize(nonce, sizeof(nonce));
            txn->waiting_session = true;
            request_session_if_idle(frame->scope.vehicle, pdom);
            if (port_bound_app()) {
                port_bound_app()->last_tx_outcome = PK_TX_NOT_SENT;
            }
            if (port_bound_ble()) {
                port_bound_ble()->last_tx_outcome = PK_TX_NOT_SENT;
            }
            return PK_OK;
        }
        txn->epoch_gen = sess->epoch_gen;
        wire_len = pk_proto_build_signed_routable(
            &req, sess, txn->req_id, s_tx_wire, sizeof(s_tx_wire));
        pk_proto_zeroize(nonce, sizeof(nonce));
        if (wire_len > 0) {
            pk_session_note_request_ex(&s_session, txn->slot, conn_gen, txn->epoch_gen,
                                       pdom, txn->routing_addr, txn->uuid,
                                       true, port_now_ms());
            pk_session_note_req_id(&s_session, txn->slot, conn_gen, pdom, txn->req_id);
        }
        if (wire_len == 0) {
            pk_session_drop(&s_session, pdom, sess);
            txn->active = false;
            diag_event(DIAG_TX_FAIL, (uint8_t)pdom, (uint8_t)frame->action, (int)PK_TX_NOT_SENT, 0);
            return tx_outcome_to_err(PK_TX_NOT_SENT);
        }
        {
            pk_tx_outcome_t outcome = gatt_write_framed(s_tx_wire, wire_len, txn->deadline);
            if (outcome != PK_TX_DELIVERED) {
                /* A counter was consumed (or a partial frame may have reached the
                 * vehicle). Drop stale session so next request re-handshakes, and
                 * preserve SentUnknown vs NotSent truthfully (S2 §2B). */
                pk_session_drop(&s_session, pdom, sess);
                txn->active = false;
                diag_event(DIAG_TX_FAIL, (uint8_t)pdom, (uint8_t)frame->action, (int)outcome, 0);
                return tx_outcome_to_err(outcome);
            }
        }
        diag_event(DIAG_SEND, (uint8_t)pdom, (uint8_t)frame->action, (int)frame->detail, 0);
        return tx_outcome_to_err(PK_TX_DELIVERED);
    }

    if (wire_len == 0) {
        txn->active = false;
        return tx_outcome_to_err(PK_TX_NOT_SENT);
    }
    {
        pk_tx_outcome_t outcome = gatt_write_framed(s_tx_wire, wire_len, txn->deadline);
        if (outcome != PK_TX_DELIVERED) {
            txn->active = false;
            return tx_outcome_to_err(outcome);
        }
    }
    pk_session_note_request_ex(&s_session, txn->slot, conn_gen, 0u, pdom,
                               txn->routing_addr, txn->uuid,
                               false, port_now_ms());
    return tx_outcome_to_err(PK_TX_DELIVERED);
}

static pk_err_t radio_cancel(void *ctx, pk_id_t txn_id)
{
    (void)ctx;
    for (size_t i = 0; i < 2; ++i) {
        if (txn_id == 0) {
            if (s_wire_txns[i].action == PK_ACTION_VCSEC_GET_STATUS_ONCE &&
                s_wire_txns[i].txn_id == 0u) {
                continue;
            }
            s_wire_txns[i].active = false;
        } else if (s_wire_txns[i].txn_id == txn_id) {
            s_wire_txns[i].active = false;
        }
    }
    return PK_OK;
}

static pk_err_t radio_connect_once(void *ctx, int vehicle_slot, pk_ms_t scan_budget_ms)
{
    const char *vin;
    char next_name[PK_PROTO_BLE_NAME_BUF];
    struct ble_gap_disc_params dparams;
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;
    int rc;

    (void)ctx;
    if (!s_ble_stack_ready) {
        return PK_ERR_NOT_WIRED;
    }
    vin = port_slot_vin(vehicle_slot);
    if (!vin || !pk_proto_vin_to_ble_name(vin, next_name)) {
        return PK_ERR_NOT_WIRED;
    }

    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (conn_h != BLE_HS_CONN_HANDLE_NONE && active_slot == vehicle_slot && tx_h != 0) {
        return PK_OK;
    }
    if (conn_h != BLE_HS_CONN_HANDLE_NONE && active_slot != vehicle_slot) {
        (void)ble_gap_terminate(conn_h, BLE_ERR_REM_USER_CONN_TERM);
    }
    diag_event(DIAG_SCAN, 0, 0, vehicle_slot, (int)(scan_budget_ms ? scan_budget_ms : PK_RECONNECT_SCAN_MS));
    if (vehicle_slot == PK_VEHICLE_PAIR_CANDIDATE &&
        s_target_slot == PK_VEHICLE_PAIR_CANDIDATE &&
        ble_gap_disc_active()) {
        return PK_OK;
    }
    portENTER_CRITICAL(&s_radio_mux);
    memcpy(s_target_ble_name, next_name, sizeof(s_target_ble_name));
    s_target_slot = vehicle_slot;
    portEXIT_CRITICAL(&s_radio_mux);

    const pk_app_t *app = port_bound_app();
    (void)ble_gap_disc_cancel();
    memset(&dparams, 0, sizeof(dparams));
    dparams.passive = 0;
    dparams.filter_duplicates = 0;
    dparams.itvl = (app && app->ap) ? 0x0060 : 0x0030;
    dparams.window = 0x0030;

    (void)ble_hs_id_infer_auto(0, &own_addr_type);
    rc = ble_gap_disc(own_addr_type,
                      (int32_t)(scan_budget_ms ? scan_budget_ms : PK_RECONNECT_SCAN_MS),
                      &dparams, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc(slot=%d) failed: %d", vehicle_slot, rc);
        return PK_ERR_NOT_WIRED;
    }
    if (vehicle_slot == PK_VEHICLE_PAIR_CANDIDATE) {
        /* Candidate slot pre-binds so pk_pair_send queues AddKey / Verify for GATT dispatch. */
        return PK_OK;
    }
    /* For saved vehicles, remain NOT_WIRED until CCCD subscribe finishes. */
    return PK_ERR_NOT_WIRED;
}

static pk_err_t radio_disconnect(void *ctx, int vehicle_slot)
{
    uint16_t conn_h;
    uint32_t new_gen;
    (void)ctx;
    (void)vehicle_slot;
    (void)ble_gap_disc_cancel();
    portENTER_CRITICAL(&s_radio_mux);
    conn_h = s_conn_handle;
    s_conn_gen++;
    if (s_conn_gen == 0u) {
        s_conn_gen = 1u;
    }
    new_gen = s_conn_gen;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_active_conn_slot = PK_VEHICLE_NONE;
    s_target_slot = PK_VEHICLE_NONE;
    s_tx_val_handle = 0;
    s_rx_val_handle = 0;
    s_ble_params_applied = false;
    pk_gatt_tx_on_disconnect(&s_gatt_tx, new_gen);
    portEXIT_CRITICAL(&s_radio_mux);
    pk_session_set_conn_gen(&s_session, new_gen);
    if (s_gatt_tx_sem) {
        xSemaphoreGive(s_gatt_tx_sem);
    }
    if (conn_h != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn_h, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_ble_rssi_valid = false;
    memset(s_wire_txns, 0, sizeof(s_wire_txns));
    return PK_OK;
}

static void on_ble_sync(void)
{
    (void)ble_hs_util_ensure_addr(0);
    s_ble_stack_ready = true;
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool verify_vehicle_sample_time(const pk_proto_session_t *sess, const pk_app_t *app,
                                       bool ts_present, int64_t ts_ms, int64_t *mono_sample_out,
                                       bool strict)
{
    int64_t veh_now_ms;
    int64_t epoch_delta_s;

    if (!sess || !sess->valid || !ts_present || ts_ms <= 0 || !app) {
        return false;
    }
    epoch_delta_s = (ts_ms / 1000LL) - (int64_t)sess->clock_time;
    if (sess->clock_time == 0 || epoch_delta_s > 86400LL || epoch_delta_s < -86400LL) {
        /* Cross-epoch (e.g. SessionInfo uptime seconds vs DriveState Unix ms):
         * the reply is bound to this live request, so arrival time is the
         * monotonic sample.
         */
        if (strict) {
            /* A clock that cannot be compared cannot prove the current gear. */
            return false;
        }
        if (mono_sample_out) {
            *mono_sample_out = (int64_t)app->now;
        }
        return true;
    }
    veh_now_ms = ((int64_t)sess->clock_time * 1000LL) +
                 (int64_t)(uint32_t)(app->now - sess->sync_local_ms);
    if (ts_ms > veh_now_ms + (int64_t)PK_CLOCK_FUTURE_TOLERANCE_MS) {
        return false;
    }
    /* VehicleData timestamps are the last *change* time of each section, not a
     * heartbeat: a car parked all night legitimately reports an hours-old
     * drive-state timestamp while answering this request right now. Do not
     * reject an old sample; the lease below starts at receipt. */
    if (mono_sample_out) {
        *mono_sample_out = (int64_t)app->now;
    }
    return true;
}

static void handle_vcsec_msg(pk_app_t *app, const uint8_t *plain, size_t plain_len,
                             pk_evidence_t evidence)
{
    pk_proto_vcsec_resp_t vresp;
    bool crypto_auth = (evidence == PK_EVIDENCE_CRYPTO_AUTHENTICATED);
    bool legacy_attr = (evidence == PK_EVIDENCE_LEGACY_ATTRIBUTED);
    if (!pk_proto_parse_vcsec_response(plain, plain_len, &vresp)) {
        return;
    }
    if (!crypto_auth && !legacy_attr) {
        return;
    }
    if (crypto_auth && vresp.has_whitelist_entry_info) {
        /* A real whitelist read. Connection and handshake do not set this. */
        app->role_observed = true;
        app->role_is_driver = (vresp.entry_key_role == (uint32_t)PK_PROTO_ROLE_DRIVER);
    }
    if (app->has_pending &&
        app->pending.kind == PK_PENDING_LOCK_VERIFY && vresp.has_vehicle_status) {
        /* S2 §1: Legacy observation may be recorded for display only, NEVER
         * promotes lock verification to confirmed (authenticated=false). */
        if (!crypto_auth) {
            pk_app_vcsec_status_observed(app, vresp.sleep_status == 2, vresp.lock_state);
        }
        pk_lock_packet_t pkt = {
            .request_id        = app->pending.id,
            .authenticated     = crypto_auth,
            .correlated        = true,
            .adapter_verified  = crypto_auth,
            .lock              = vresp.lock_state,
            .closures          = vresp.closures,
            .closures_complete = vresp.has_closure_statuses,
            .complete          = true,
        };
        (void)pk_app_lock_packet(app, &pkt);
        return;
    }
    if (vresp.has_vehicle_status) {
        if (crypto_auth) {
            pk_app_vcsec_status(app, vresp.sleep_status == 2, vresp.lock_state);
        } else {
            /* Display-only observation; never sets vcsec_status_valid or
             * feeds pk_app_can_vcsec_unlock / unlock confirmation. */
            pk_app_vcsec_status_observed(app, vresp.sleep_status == 2, vresp.lock_state);
        }
        if (!app->has_pending) {
            return;
        }
    }
    if (app->has_pending &&
        app->pending.kind == PK_PENDING_WAKE_THEN && app->pending.qphase == 0) {
        if ((vresp.has_command_status && vresp.op_status == 0) || vresp.has_vehicle_status) {
            if (crypto_auth || (app->wake_then_status && !app->has_deferred)) {
                /* S2 §1: Under an already-authorized bounded wake intent, a
                 * legacy wake ACK may advance read-only status follow-up once
                 * (no extra wake budget, no write intent). */
                app->pending.qphase = 1;
                app->pending.deadline = pk_app_phase_deadline(app, app->now, PK_WAKE_SETTLE_MS);
            } else {
                /* Never trigger a deferred WRITE intent from a weak wake ACK. */
                app->has_deferred = false;
                app->intent_deadline = 0;
                app->wake_then_status = false;
                pk_ble_release_in_flight(app);
                app->has_pending = false;
                pk_app_set_busy(app, false);
                pk_app_set_msg(app, PK_MSG_WAKE_FAIL);
            }
        } else if (vresp.has_command_status && vresp.op_status == 2) {
            app->has_deferred = false;
            app->intent_deadline = 0;
            app->wake_then_status = false;
            pk_ble_release_in_flight(app);
            app->has_pending = false;
            pk_app_set_busy(app, false);
            pk_app_set_msg(app, PK_MSG_UNQUALIFIED);
        }
        return;
    }
    if (!app->has_pending) {
        return;
    }
    if (app->pending.kind == PK_PENDING_QUALIFICATION &&
        app->pending.qphase == PK_QPHASE_WAKE_WAIT) {
        if ((vresp.has_command_status && vresp.op_status == 0) || vresp.has_vehicle_status) {
            if (crypto_auth) {
                (void)pk_app_qualification_wake_result(app, app->pending.id,
                                                       &app->pending.scope, true, true);
            } else {
                /* S2 §1: Bounded read-only qualification follow-up on legacy wake ACK. */
                (void)pk_app_qualification_wake_legacy_ack(app, app->pending.id,
                                                           &app->pending.scope);
            }
        }
        return;
    }
    if (vresp.has_nominal_error) {
        if (crypto_auth) {
            (void)pk_app_command_result(app, app->pending.id, PK_CMD_RESULT_REJECTED, true);
        } else {
            (void)pk_app_command_legacy_receipt(app, app->pending.id, PK_CMD_RESULT_REJECTED);
        }
        return;
    }
    if (vresp.has_command_status) {
        pk_cmd_result_t r = PK_CMD_RESULT_UNCONFIRMED;
        if (vresp.op_status == 1) {
            r = PK_CMD_RESULT_WAIT;
        } else if (vresp.op_status == 0) {
            r = PK_CMD_RESULT_ACCEPTED;
        } else if (vresp.op_status == 2) {
            /* Official A19: bare OPERATIONSTATUS_ERROR on non-whitelist ops is
             * for legacy clients; drop it and keep waiting for nominalError. */
            if (!vresp.has_whitelist_status && vresp.signed_msg_fault == 0) {
                return;
            }
            r = PK_CMD_RESULT_REJECTED;
        } else {
            return;
        }
        if (crypto_auth) {
            (void)pk_app_command_result(app, app->pending.id, r, true);
        } else {
            (void)pk_app_command_legacy_receipt(app, app->pending.id, r);
        }
    } else if (vresp.has_vehicle_status) {
        if (crypto_auth) {
            (void)pk_app_command_result(app, app->pending.id, PK_CMD_RESULT_ACCEPTED, true);
        } else {
            (void)pk_app_command_legacy_receipt(app, app->pending.id, PK_CMD_RESULT_UNCONFIRMED);
        }
    } else if (plain_len == 0u && crypto_auth && app->pending.kind == PK_PENDING_COMMAND) {
        /* Official A19: empty authenticated FromVCSECMessage is terminal success
         * for non-whitelist VCSEC commands (LOCK/UNLOCK still enter read-only verify). */
        (void)pk_app_command_result(app, app->pending.id, PK_CMD_RESULT_ACCEPTED, true);
    }
}

static void apply_infotain_caps(pk_app_t *app, const pk_proto_carserver_resp_t *cresp)
{
    if (cresp->has_vehicle_data) {
        app->caps.adapter_verified = true;
    }
    if (cresp->sentry_present) {
        app->caps.sentry_known = true;
        app->caps.sentry_armed = cresp->sentry_armed;
    }
    if (cresp->has_charge_state) {
        if (cresp->cable_connected_present) {
            app->caps.cable_connected = cresp->cable_connected ? PK_CAP_YES : PK_CAP_NO;
        }
        if (cresp->charge_port_door_open_present) {
            app->caps.port_open = cresp->charge_port_door_open ? PK_CAP_YES : PK_CAP_NO;
            app->caps.action[PK_ACTION_CHARGE_PORT_OPEN] = PK_CAP_YES;
            app->caps.action[PK_ACTION_CHARGE_PORT_CLOSE] = PK_CAP_YES;
        }
    }
    if (cresp->has_climate_state) {
        int i;
        app->caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
        app->caps.action[PK_ACTION_PRECONDITION_MAX_ON] = PK_CAP_YES;
        app->caps.action[PK_ACTION_SET_SEAT_HEAT] = PK_CAP_YES;
        app->caps.action[PK_ACTION_SET_STEERING_HEAT] = PK_CAP_YES;
        app->caps.seat_position_count = 5;
        for (i = 0; i < 5; i++) {
            app->caps.seat_positions[i] = i;
        }
        if (cresp->is_climate_on_present) {
            app->caps.climate_on = cresp->is_climate_on ? PK_CAP_YES : PK_CAP_NO;
        }
        if (cresp->is_preconditioning_present) {
            app->caps.preconditioning =
                cresp->is_preconditioning ? PK_CAP_YES : PK_CAP_NO;
        }
    }
    if (cresp->has_drive_state || cresp->locked_present) {
        app->caps.action[PK_ACTION_LOCK] = PK_CAP_YES;
        app->caps.action[PK_ACTION_UNLOCK] = PK_CAP_YES;
        app->caps.action[PK_ACTION_FRUNK_OPEN] = PK_CAP_YES;
        app->caps.action[PK_ACTION_TRUNK_MOVE] = PK_CAP_YES;
        app->caps.action[PK_ACTION_FLASH] = PK_CAP_YES;
        app->caps.action[PK_ACTION_HONK] = PK_CAP_YES;
        app->caps.action[PK_ACTION_WINDOW_VENT] = PK_CAP_YES;
    }
}

static void apply_infotain_status_fields(pk_app_t *app, const pk_proto_session_t *sess,
                                         const pk_proto_carserver_resp_t *cresp,
                                         bool crypto_auth)
{
    static const char *const k_tpms_keys[4] = { "tpms_fl", "tpms_fr", "tpms_rl", "tpms_rr" };
    const pk_scope_t *sc = &app->pending.scope;
    pk_id_t rid = app->pending.id;
    int64_t chg_sample = 0;
    int64_t cli_sample = 0;
    int64_t cls_sample = 0;
    int64_t tpms_sample = 0;
    bool chg_clk = crypto_auth && verify_vehicle_sample_time(sess, app, cresp->charge_ts_present, cresp->charge_ts_ms, &chg_sample, false);
    bool cli_clk = crypto_auth && verify_vehicle_sample_time(sess, app, cresp->climate_ts_present, cresp->climate_ts_ms, &cli_sample, false);
    bool cls_clk = crypto_auth && verify_vehicle_sample_time(sess, app, cresp->closures_ts_present, cresp->closures_ts_ms, &cls_sample, false);
    bool tpms_clk = crypto_auth && verify_vehicle_sample_time(sess, app, cresp->tpms_ts_present, cresp->tpms_ts_ms, &tpms_sample, false);
    int range_tenths_km = (cresp->battery_range_tenths_mi * 1609 + 500) / 1000;
    const struct {
        bool        present;
        const char *key;
        int         val;
        bool        unit_ok;
        bool        ts_present;
        int64_t     sample;
        bool        clk_ok;
    } k_fields[] = {
        { cresp->battery_level_present,    "soc",          cresp->battery_level,        true, cresp->charge_ts_present,   chg_sample, chg_clk },
        { cresp->battery_range_present,    "range",        range_tenths_km,             true, cresp->charge_ts_present,   chg_sample, chg_clk },
        { cresp->charging_state > 0,       "charge_state", cresp->charging_state,       true, cresp->charge_ts_present,   chg_sample, chg_clk },
        { cresp->minutes_to_limit_present, "eta_limit",    cresp->minutes_to_limit,        true,  cresp->charge_ts_present,   chg_sample, chg_clk },
        { cresp->minutes_to_full_present,  "eta_full",     cresp->minutes_to_full,         true,  cresp->charge_ts_present,   chg_sample, chg_clk },
        { cresp->inside_temp_present,      "cabin_temp",   cresp->inside_temp_tenths_c,    true,  cresp->climate_ts_present,  cli_sample, cli_clk },
        { cresp->locked_present,           "locked",       cresp->locked ? 1 : 0,          true,  cresp->closures_ts_present, cls_sample, cls_clk },
    };
    size_t i;

    for (i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); ++i) {
        if (k_fields[i].present) {
            pk_status_input_t in = {
                .request_id     = rid,
                .scope          = sc,
                .name           = k_fields[i].key,
                .value          = k_fields[i].val,
                .present        = true,
                .sample_present = k_fields[i].ts_present,
                .sampled        = k_fields[i].sample,
                .unit_verified  = k_fields[i].unit_ok,
                .clock_verified = k_fields[i].clk_ok,
            };
            (void)pk_app_status_field(app, &in);
        }
    }
    for (i = 0; i < 4; ++i) {
        if (cresp->tpms_present[i]) {
            pk_status_input_t in = {
                .request_id     = rid,
                .scope          = sc,
                .name           = k_tpms_keys[i],
                .value          = cresp->tpms_mbar[i],
                .present        = true,
                .sample_present = cresp->tpms_ts_present,
                .sampled        = tpms_sample,
                .unit_verified  = true,
                .clock_verified = tpms_clk,
            };
            (void)pk_app_status_field(app, &in);
        }
    }
    /* One section per request: send the next section or finish the page. */
    (void)pk_app_status_advance(app);
}

static void handle_infotain_msg(pk_app_t *app, const pk_proto_session_t *sess,
                                const uint8_t *plain, size_t plain_len,
                                pk_evidence_t evidence)
{
    pk_proto_carserver_resp_t cresp;
    /* S2 §1 + D03: Legacy plaintext (PK_EVIDENCE_LEGACY_ATTRIBUTED) may populate
     * non-authoritative status display fields (clock_verified=false) and serve
     * as an honest unconfirmed command receipt, but MUST NEVER set
     * adapter_verified, mutate capabilities, or grant Park qualification. */
    bool crypto_auth = (evidence == PK_EVIDENCE_CRYPTO_AUTHENTICATED);
    bool legacy_attr = (evidence == PK_EVIDENCE_LEGACY_ATTRIBUTED);
    if (!pk_proto_parse_carserver_response(plain, plain_len, &cresp) ||
        !app->has_pending || (!crypto_auth && !legacy_attr)) {
        return;
    }

    if (crypto_auth) {
        apply_infotain_caps(app, &cresp);
    }

    if (app->pending.kind == PK_PENDING_QUALIFICATION &&
        app->pending.qphase == PK_QPHASE_READ_WAIT && cresp.has_vehicle_data) {
        bool speed_bad = cresp.has_drive_state &&
                         (cresp.speed_invalid || cresp.speed_float_invalid);
        pk_qualify_sample_t qs;
        pk_qualify_fill_drive(&qs, app->pending.id, &app->pending.scope,
                              cresp.has_drive_state, cresp.shift,
                              cresp.speed_present, cresp.speed_mph,
                              cresp.speed_float_present, cresp.speed_float_mph,
                              speed_bad, cresp.locked_present, cresp.locked,
                              crypto_auth, (int64_t)app->now);
        ESP_LOGI(TAG, "qualify rx: drv=%d shift=%d spd=%d bad=%d live=%d",
                 cresp.has_drive_state ? 1 : 0,
                 cresp.has_drive_state ? (int)cresp.shift : -1,
                 (cresp.has_drive_state && cresp.speed_present) ? cresp.speed_mph : -1,
                 speed_bad ? 1 : 0,
                 crypto_auth ? 1 : 0);
        diag_event(DIAG_QUAL, 0, (uint8_t)qs.shift,
                   (crypto_auth ? 1 : 0) | (crypto_auth ? 2 : 0) |
                       (qs.speed_present ? 4 : 0) | (qs.sample_present ? 8 : 0),
                   0);
        (void)pk_app_qualification_result(app, &qs);
        return;
    }

    if (app->pending.kind == PK_PENDING_STATUS && cresp.has_vehicle_data) {
        apply_infotain_status_fields(app, sess, &cresp, crypto_auth);
        return;
    }

    if (cresp.has_action_status) {
        pk_cmd_result_t r = (cresp.result == 0) ? PK_CMD_RESULT_ACCEPTED : PK_CMD_RESULT_REJECTED;
        if (crypto_auth) {
            (void)pk_app_command_result(app, app->pending.id, r, true);
        } else {
            (void)pk_app_command_legacy_receipt(app, app->pending.id, r);
        }
    }
}

/* ==========================================================================
 * 5. Incoming BLE Frame Dispatch into Audited pk_app State Machine
 * ========================================================================== */

static void advance_pairing_from_session(pk_app_t *app, int slot, pk_proto_domain_t from_domain)
{
    if (app->page != PK_PAGE_PAIR) {
        return;
    }
    if (app->pair_step == 3 && !app->approved &&
        from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY) {
        /* Verified SessionInfo(STATUS_OK) proves the vehicle added our public
         * key to its whitelist after the user tapped the NFC card in Step 3.
         */
        (void)pk_app_pair_approve(app, true, app->pair_id, true);
        return;
    }
    if (app->pair_step != 4) {
        return;
    }
    if (from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY) {
        s_wire_txns[0].pair_vcsec_ok = true;
        if (!s_wire_txns[0].pair_infotain_ok) {
            send_domain_session_request(slot, PK_PROTO_DOMAIN_INFOTAINMENT);
        }
    } else if (from_domain == PK_PROTO_DOMAIN_INFOTAINMENT) {
        s_wire_txns[0].pair_infotain_ok = true;
    }
    if (s_wire_txns[0].pair_vcsec_ok && s_wire_txns[0].pair_infotain_ok) {
        pk_pair_verify_t v = {
            .request_id                 = app->pair_id,
            .vcsec_authenticated        = true,
            .infotainment_authenticated = true,
            .key_matches                = true,
            .role_observed              = false,
            .role_driver                = false,
            .identity_binding_verified  = false,
        };
        (void)pk_app_pair_verify(app, &v);
    }
}

static void flush_waiting_session_txn(pk_app_t *app, const pk_proto_key_slot_t *ks,
                                      const char *vin, pk_proto_session_t *sess,
                                      pk_proto_domain_t from_domain, uint32_t conn_gen)
{
    active_wire_txn_t *txn = wire_txn_for_domain(from_domain);
    uint8_t nonce[12];
    uint8_t wire[384];
    size_t wlen;

    pk_proto_signed_req_t req = {
        .route         = { txn->domain, txn->routing_addr, txn->uuid },
        .vin           = vin,
        .client_pubkey = ks->pubkey,
        .inner_payload = txn->inner,
        .inner_len     = txn->inner_len,
        .nonce         = nonce,
        .now_ms        = app->now,
        .ttl_seconds   = 5u,
    };

    if (!txn->active || !txn->waiting_session ||
        txn->domain != from_domain || txn->conn_gen != conn_gen) {
        return;
    }
    txn->waiting_session = false;
    if (pk_time_due(app->now, txn->deadline)) {
        pk_id_t tid = txn->txn_id;
        txn->active = false;
        (void)pk_app_tx_failed(app, tid, PK_TX_NOT_SENT);
        return;
    }
    if (!pk_proto_random_bytes(nonce, sizeof(nonce))) {
        pk_id_t tid = txn->txn_id;
        pk_proto_zeroize(nonce, sizeof(nonce));
        txn->active = false;
        ESP_LOGW(TAG, "RNG unavailable for flush_waiting_session_txn; fail-closed");
        (void)pk_app_tx_failed(app, tid, PK_TX_NOT_SENT);
        return;
    }
    txn->epoch_gen = sess->epoch_gen;
    pk_proto_rx_window_init(&txn->rx_window);
    wlen = pk_proto_build_signed_routable(&req, sess, txn->req_id, wire, sizeof(wire));
    pk_proto_zeroize(nonce, sizeof(nonce));
    if (wlen > 0) {
        pk_session_note_request_ex(&s_session, txn->slot, conn_gen, txn->epoch_gen,
                                   from_domain, txn->routing_addr, txn->uuid,
                                   true, port_now_ms());
        pk_session_note_req_id(&s_session, txn->slot, conn_gen, from_domain, txn->req_id);
    }
    if (wlen == 0) {
        pk_id_t tid = txn->txn_id;
        pk_session_drop(&s_session, from_domain, sess);
        txn->active = false;
        (void)pk_app_tx_failed(app, tid, PK_TX_NOT_SENT);
        return;
    }
    {
        pk_tx_outcome_t outcome = gatt_write_framed(wire, wlen, txn->deadline);
        if (port_bound_ble()) {
            port_bound_ble()->last_tx_outcome = outcome;
        }
        if (outcome != PK_TX_DELIVERED) {
            pk_id_t tid = txn->txn_id;
            pk_session_drop(&s_session, from_domain, sess);
            txn->active = false;
            /* S2 §2B: Notify orchestration on flush failure so pending never
             * hangs and SentUnknown vs NotSent evidence is preserved. */
            (void)pk_app_tx_failed(app, tid, outcome);
            return;
        }
        app->last_tx_outcome = PK_TX_DELIVERED;
        if (app->has_pending && app->pending.kind == PK_PENDING_COMMAND &&
            app->pending.id == txn->txn_id) {
            pk_app_set_msg(app, PK_MSG_SENT_WAIT);
        }
    }
}

/* Refresh VCSEC lock state once after a session becomes ready (fresh handshake
 * or a reused surviving session). VCSEC replies may omit request_uuid, so
 * this probe is attributed by its single in-flight VCSEC txn (conn_gen +
 * epoch_gen + routing address + deadline) inside pk_session_accept_rx(). */
static void start_background_status_probe(pk_app_t *app, int slot, uint32_t conn_gen,
                                          pk_proto_key_slot_t *ks, const char *vin)
{
    uint8_t inner[32];
    size_t inner_len;
    active_wire_txn_t *txn;
    uint8_t nonce[12], wire[256];
    size_t wlen;
    pk_proto_signed_req_t req;

    if (!app || !ks || !vin || slot != app->vehicle ||
        app->has_pending || app->busy || app->vcsec_status_valid ||
        !ks->vcsec.valid || s_wire_txns[0].active) {
        return;
    }
    inner_len = pk_proto_encode_action_payload(
        PK_ACTION_VCSEC_GET_STATUS_ONCE, NULL, ks->pubkey, inner, sizeof(inner));
    if (inner_len == 0) {
        return;
    }
    txn = wire_txn_for_domain(PK_PROTO_DOMAIN_VEHICLE_SECURITY);
    memset(txn, 0, sizeof(*txn));
    txn->active = true;
    txn->txn_id = 0u;
    txn->action = PK_ACTION_VCSEC_GET_STATUS_ONCE;
    txn->domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    txn->slot = slot;
    txn->conn_gen = conn_gen;
    txn->epoch_gen = ks->vcsec.epoch_gen;
    txn->scope = pk_app_scope(app);
    txn->deadline = app->now + pk_plan_deadline(PK_PLAN_STATUS_SECTION);
    pk_proto_rx_window_init(&txn->rx_window);
    if (!pk_proto_random_bytes(txn->routing_addr, sizeof(txn->routing_addr)) ||
        !pk_proto_random_bytes(txn->uuid, sizeof(txn->uuid)) ||
        !pk_proto_random_bytes(nonce, sizeof(nonce))) {
        pk_proto_zeroize(nonce, sizeof(nonce));
        txn->active = false;
        ESP_LOGW(TAG, "RNG unavailable for status probe; fail-closed");
        return;
    }
    req = (pk_proto_signed_req_t){
        .route         = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, txn->routing_addr, txn->uuid },
        .vin           = vin,
        .client_pubkey = ks->pubkey,
        .inner_payload = inner,
        .inner_len     = inner_len,
        .nonce         = nonce,
        .now_ms        = app->now,
        .ttl_seconds   = 5u,
    };
    wlen = pk_proto_build_signed_routable(&req, &ks->vcsec, txn->req_id, wire, sizeof(wire));
    pk_proto_zeroize(nonce, sizeof(nonce));
    if (wlen > 0) {
        pk_session_note_request_ex(&s_session, txn->slot, conn_gen, txn->epoch_gen,
                                   PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                   txn->routing_addr, txn->uuid, true, port_now_ms());
        pk_session_note_req_id(&s_session, txn->slot, conn_gen,
                               PK_PROTO_DOMAIN_VEHICLE_SECURITY, txn->req_id);
    }
    if (wlen == 0 || gatt_write_framed(wire, wlen, txn->deadline) != PK_TX_DELIVERED) {
        pk_session_drop(&s_session, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &ks->vcsec);
        txn->active = false;
    }
}

static bool apply_rx_session_info(pk_app_t *app, int slot, uint32_t conn_gen,
                                  pk_proto_key_slot_t *ks,
                                  const char *vin, const pk_proto_routable_t *rmsg)
{
    pk_proto_session_t *sess;
    pk_session_apply_t apply;
    uint32_t prev_epoch_gen;
    bool was_valid;

    if (!app || !ks || !vin || !rmsg) {
        return false;
    }
    sess = (rmsg->from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY)
               ? &ks->vcsec
               : &ks->infotainment;
    prev_epoch_gen = sess->epoch_gen;
    was_valid = sess->valid;
    apply = (pk_session_apply_t){
        .slot           = slot,
        .conn_gen       = conn_gen,
        .domain         = rmsg->from_domain,
        .vin            = vin,
        .privkey        = ks->privkey,
        .rmsg           = rmsg,
        .now_ms         = port_now_ms(),
        .session_now_ms = app->now,
    };
    if (!pk_session_apply(&s_session, &apply, sess)) {
        return false;
    }
    if (was_valid && sess->epoch_gen != prev_epoch_gen) {
        /* Epoch transitioned: invalidate any already-transmitted txn on this
         * domain so late responses from the prior epoch cannot complete it. */
        active_wire_txn_t *dtxn = wire_txn_for_domain(rmsg->from_domain);
        if (dtxn->active && !dtxn->waiting_session && dtxn->epoch_gen != sess->epoch_gen) {
            dtxn->active = false;
        }
    }
    ESP_LOGI(TAG, "session ok dom=%d counter=%u clock=%u egen=%u",
             (int)rmsg->from_domain, (unsigned)sess->counter,
             (unsigned)sess->clock_time, (unsigned)sess->epoch_gen);
    diag_event(DIAG_SESSION, (uint8_t)rmsg->from_domain, 0,
               (int)(sess->counter & 0xFFu), (int)(sess->clock_time & 0x7FFFu));
    if (app->page == PK_PAGE_PAIR) {
        advance_pairing_from_session(app, slot, rmsg->from_domain);
        return true;
    }
    flush_waiting_session_txn(app, ks, vin, sess, rmsg->from_domain, conn_gen);
    if (rmsg->from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY && slot == app->vehicle) {
        start_background_status_probe(app, slot, conn_gen, ks, vin);
    }
    return true;
}

static bool drop_unmatched_passive_auth(size_t frame_len, const pk_proto_routable_t *rmsg)
{
    const uint8_t *live[2];
    int n = 0;
    int i;

    if (!rmsg) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (s_wire_txns[i].conn_gen != 0u) {
            live[n++] = s_wire_txns[i].routing_addr;
        }
    }
    if (n == 0) {
        return pk_session_passive_auth_unmatched(&s_session, frame_len, rmsg, NULL);
    }
    for (i = 0; i < n; i++) {
        if (!pk_session_passive_auth_unmatched(&s_session, frame_len, rmsg, live[i])) {
            return false;
        }
    }
    return true;
}

static void handle_rx_frame(pk_app_t *app, int slot, uint32_t conn_gen,
                            const uint8_t *buf, size_t len)
{
    static uint8_t s_rx_plain[PK_PROTO_MAX_FRAME_LEN];
    pk_proto_routable_t rmsg;
    pk_proto_key_slot_t *ks = port_slot_keys(slot);
    const char *vin = port_slot_vin(slot);
    pk_proto_session_t *sess;
    active_wire_txn_t *txn;
    size_t plain_len = 0;
    pk_evidence_t evidence = PK_EVIDENCE_UNKNOWN;

    if (!app || !ks || !vin || !buf || len == 0) {
        return;
    }

    /* Isolated pre-session pairing AddKey Step 3 entry (D03):
     * Accepts WhitelistOperationStatus ONLY while pairing step 3 is active and
     * no VCSEC session exists yet. Never enters normal operational dispatch. */
    if (app->page == PK_PAGE_PAIR && app->pair_step == 3 && !ks->vcsec.valid) {
        pk_proto_vcsec_resp_t pair_vresp;
        pk_evidence_t pair_ev = PK_EVIDENCE_UNKNOWN;
        if (pk_session_accept_pre_session_pair(true, ks->vcsec.valid,
                                               buf, len, &pair_vresp, &pair_ev)) {
            ESP_LOGI(TAG, "pair whitelist status: op=%d info=%d fault=%d ev=%d",
                     pair_vresp.op_status, pair_vresp.whitelist_info,
                     pair_vresp.signed_msg_fault, (int)pair_ev);
            if (pair_vresp.op_status == 0 && pair_vresp.whitelist_info == 0) {
                (void)pk_app_pair_approve(app, true, app->pair_id, true);
            } else if (pair_vresp.op_status == 2) {
                (void)pk_app_pair_approve(app, false, app->pair_id, true);
            }
            return;
        }
    }

    if (!pk_proto_parse_routable(buf, len, &rmsg)) {
        return;
    }
    /* After connect, a 31/41-byte passive auth frame for some other routing
     * address is discarded here, before session apply or the anti-replay window. */
    if (drop_unmatched_passive_auth(len, &rmsg)) {
        return;
    }

    {
        bool session_ok = apply_rx_session_info(app, slot, conn_gen, ks, vin, &rmsg);
        if (!session_ok && rmsg.has_session_info) {
            diag_event(DIAG_SESSION_FAIL, (uint8_t)rmsg.from_domain, 0, 0, 0);
            /* S2 §2C: A packet carrying a malformed/bad-HMAC SessionInfo MUST
             * NOT execute any fault or payload side effects. */
            return;
        }
        if (rmsg.has_msg_status && rmsg.signed_message_fault != 0) {
            int fault = rmsg.signed_message_fault;
            pk_proto_domain_t dom = rmsg.from_domain;
            pk_proto_session_t *fsess;
            active_wire_txn_t *ftxn;
            bool fault_authenticated = false;

            if (!rmsg.has_from_domain ||
                (dom != PK_PROTO_DOMAIN_VEHICLE_SECURITY &&
                 dom != PK_PROTO_DOMAIN_INFOTAINMENT)) {
                return;
            }
            fsess = (dom == PK_PROTO_DOMAIN_VEHICLE_SECURITY)
                        ? &ks->vcsec
                        : &ks->infotainment;
            ftxn = wire_txn_for_domain(dom);

            /* S2 §2C: If the same packet carries an AEAD or non-SessionInfo
             * signature block alongside a fault, authenticate it FIRST; a
             * malformed/bad-tag signature drops the whole packet with ZERO
             * fault side effects. */
            if (rmsg.has_aes_gcm_resp || (rmsg.has_sig_data && !rmsg.has_session_info_tag)) {
                pk_session_rx_in_t rx_chk = {
                    .slot             = slot,
                    .conn_gen         = conn_gen,
                    .domain           = dom,
                    .vin              = vin,
                    .sess             = fsess,
                    .rmsg             = &rmsg,
                    .now_ms           = app->now,
                    .txn_active       = ftxn->active,
                    .txn_slot         = ftxn->slot,
                    .txn_conn_gen     = ftxn->conn_gen,
                    .txn_epoch_gen    = ftxn->epoch_gen,
                    .txn_deadline     = ftxn->deadline,
                    .txn_routing_addr = ftxn->routing_addr,
                    .txn_uuid         = ftxn->uuid,
                    .txn_req_id       = ftxn->req_id,
                    .rx_window        = pk_session_rx_window_for_req(&s_session, dom, ftxn->req_id),
                    .frame_len        = len,
                };
                pk_evidence_t chk_ev = PK_EVIDENCE_UNKNOWN;
                size_t chk_len = 0;
                if (!rmsg.has_payload_bytes ||
                    !pk_session_accept_rx(&s_session, &rx_chk, s_rx_plain, sizeof(s_rx_plain),
                                          &chk_len, &chk_ev) ||
                    chk_ev != PK_EVIDENCE_CRYPTO_AUTHENTICATED) {
                    return;
                }
                fault_authenticated = true;
            }

            ESP_LOGW(TAG, "vehicle fault dom=%d code=%d action=%d auth=%d",
                     (int)dom, fault, (int)ftxn->action, fault_authenticated ? 1 : 0);
            diag_event(DIAG_FAULT, (uint8_t)dom, (uint8_t)ftxn->action, fault, 0);
            /* Strict fault attribution: slot + conn_gen + domain + routing_addr +
             * optional UUID + deadline. SessionInfo HMAC never endorses an outer fault. */
            if (!pk_session_fault_gate_ex(&s_session, slot, conn_gen, dom, &rmsg,
                                          app->now, port_now_ms(),
                                          ftxn->active, ftxn->slot, ftxn->conn_gen,
                                          ftxn->deadline, ftxn->uuid, ftxn->routing_addr,
                                          ftxn->action, fault)) {
                return;
            }
            (void)pk_session_clear_on_fault(&s_session, dom, fsess, fault, session_ok);
            memset(ftxn, 0, sizeof(*ftxn));
            pk_app_protocol_fault_ex(app, fault, dom == PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     fault_authenticated);
            return;
        }
    }
    if (!rmsg.has_payload_bytes) {
        return;
    }
    if (rmsg.from_domain != PK_PROTO_DOMAIN_VEHICLE_SECURITY &&
        rmsg.from_domain != PK_PROTO_DOMAIN_INFOTAINMENT) {
        return;
    }

    sess = (rmsg.from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY)
               ? &ks->vcsec
               : &ks->infotainment;
    txn = wire_txn_for_domain(rmsg.from_domain);

    /* Single authoritative RX acceptance kernel (D02 + D03):
     * attribution (slot + conn_gen + epoch_gen + domain + routing_addr +
     * optional uuid + deadline) -> AEAD decrypt -> official width-32 sliding-window
     * commit -> explicit evidence classification. */
    {
        pk_session_rx_in_t rx_in = {
            .slot             = slot,
            .conn_gen         = conn_gen,
            .domain           = rmsg.from_domain,
            .vin              = vin,
            .sess             = sess,
            .rmsg             = &rmsg,
            .now_ms           = app->now,
            .txn_active       = txn->active,
            .txn_slot         = txn->slot,
            .txn_conn_gen     = txn->conn_gen,
            .txn_epoch_gen    = txn->epoch_gen,
            .txn_deadline     = txn->deadline,
            .txn_routing_addr = txn->routing_addr,
            .txn_uuid         = txn->uuid,
            .txn_req_id       = txn->req_id,
            .rx_window        = pk_session_rx_window_for_req(&s_session, rmsg.from_domain, txn->req_id),
            .frame_len        = len,
        };
        if (!pk_session_accept_rx(&s_session, &rx_in, s_rx_plain, sizeof(s_rx_plain),
                                  &plain_len, &evidence)) {
            /* Observation track: accept AEAD-authenticated vehicle state
             * snapshots on the active domain session even when no command txn
             * is active or after a txn has ended, without completing any
             * unrelated pending command. */
            pk_session_obs_in_t obs_in = {
                .slot     = slot,
                .conn_gen = conn_gen,
                .domain   = rmsg.from_domain,
                .vin      = vin,
                .sess     = sess,
                .rmsg     = &rmsg,
                .frame_len = len,
                .endpoint_routing = (txn->conn_gen != 0u) ? txn->routing_addr : NULL,
            };
            if (slot != app->vehicle || slot == PK_VEHICLE_NONE ||
                !pk_session_accept_observation_rx(&s_session, &obs_in,
                                                  s_rx_plain, sizeof(s_rx_plain),
                                                  &plain_len, &evidence) ||
                plain_len == 0) {
                return;
            }
            if (rmsg.from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY) {
                pk_proto_vcsec_resp_t vresp;
                if (pk_proto_parse_vcsec_response(s_rx_plain, plain_len, &vresp) &&
                    vresp.has_vehicle_status) {
                    pk_app_vcsec_status(app, vresp.sleep_status == 2, vresp.lock_state);
                }
            } else if (rmsg.from_domain == PK_PROTO_DOMAIN_INFOTAINMENT) {
                pk_proto_carserver_resp_t cresp;
                if (pk_proto_parse_carserver_response(s_rx_plain, plain_len, &cresp) &&
                    cresp.has_vehicle_data) {
                    apply_infotain_caps(app, &cresp);
                }
            }
            return;
        }
    }
    if (plain_len == 0 &&
        !(rmsg.from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY &&
          evidence == PK_EVIDENCE_CRYPTO_AUTHENTICATED &&
          app->has_pending && app->pending.kind == PK_PENDING_COMMAND)) {
        return;
    }
    if (evidence == PK_EVIDENCE_CRYPTO_AUTHENTICATED) {
        diag_event(DIAG_HINT, (uint8_t)rmsg.from_domain, (uint8_t)txn->action,
                   (int)plain_len, 0);
    } else if (evidence == PK_EVIDENCE_LEGACY_ATTRIBUTED) {
        diag_event(DIAG_RX_PLAINTEXT, (uint8_t)rmsg.from_domain, (uint8_t)txn->action,
                   (int)plain_len, 0);
    }

    /* Keep the decoded inner payload (last few frames) for a no-PC post-mortem. */
    diag_raw(1, (uint8_t)rmsg.from_domain, (uint8_t)txn->action, s_rx_plain, plain_len);
    if (rmsg.from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY) {
        bool was_bg_probe = (!app->has_pending && txn->active &&
                             txn->action == PK_ACTION_VCSEC_GET_STATUS_ONCE);
        handle_vcsec_msg(app, s_rx_plain, plain_len, evidence);
        if (was_bg_probe) {
            txn->active = false;
        }
    } else if (rmsg.from_domain == PK_PROTO_DOMAIN_INFOTAINMENT) {
        handle_infotain_msg(app, sess, s_rx_plain, plain_len, evidence);
    }
}

static void maybe_update_ble_conn_params(const pk_app_t *app)
{
    struct ble_gap_upd_params params;
    bool want_fast;
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;

    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (!app || conn_h == BLE_HS_CONN_HANDLE_NONE || tx_h == 0 ||
        active_slot == PK_VEHICLE_NONE || active_slot == PK_VEHICLE_PAIR_CANDIDATE) {
        return;
    }
    want_fast = (app->power == PK_POWER_COMFORT);
    if (s_ble_params_applied && want_fast == s_ble_last_fast) {
        return;
    }
    if (s_conn_ready_ms == 0 || !pk_time_elapsed(port_now_ms(), s_conn_ready_ms, 1500u)) {
        return;
    }
    memset(&params, 0, sizeof(params));
    if (want_fast) {
        params.itvl_min = 24;   /* 30 ms */
        params.itvl_max = 48;   /* 60 ms */
        params.latency = 0;
    } else {
        params.itvl_min = 80;   /* 100 ms */
        params.itvl_max = 160;  /* 200 ms */
        params.latency = 2;
    }
    params.supervision_timeout = 600; /* 6000 ms */
    if (ble_gap_update_params(conn_h, &params) == 0) {
        s_ble_params_applied = true;
        s_ble_last_fast = want_fast;
    }
}

void port_ble_start_candidate_pairing(pk_app_t *app, const char *vin)
{
    char parsed[PK_VIN_BUF];

    if (!app) {
        return;
    }
    /* SoftAP submit is the normal VIN-page path. After an explicit RNG/keygen
     * reject the portal is already closed, but the same unused candidate VIN
     * remains in the session; allow that retry to generate a key and enter
     * pairing once entropy is restored. Do not invent a VIN if none was
     * accepted in this session. */
    if (app->ap) {
        if (!pk_app_submit_vin(app, vin)) {
            return;
        }
    } else if (!vin || !pk_vin_valid(vin, parsed) || !app->has_vin ||
               strcmp(app->vin, parsed) != 0) {
        /* The hotspot is gone and this page cannot mint a VIN. */
        pk_app_set_msg(app, PK_MSG_AP_EXPIRED);
        return;
    }
    /* S2 §2E: If RNG/keygen fails for the candidate keypair, block AddKey &
     * pairing entry explicitly instead of sending a zero pubkey or quietly
     * timing out in Step 3. */
    if (!port_ensure_slot_keypair(PK_VEHICLE_PAIR_CANDIDATE)) {
        ESP_LOGE(TAG, "candidate pairing blocked: RNG/keygen failed");
        diag_event(DIAG_TX_FAIL, (uint8_t)PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                   (uint8_t)PK_ACTION_ADDKEY, (int)PK_TX_NOT_SENT, 0);
        pk_app_set_msg(app, PK_MSG_PAIR_REJECTED);
        return;
    }
    if (pk_app_pair_start(app, app->vin, true, true)) {
        /* Pre-scan and connect to the candidate vehicle while user reads Step 1/2 */
        (void)radio_connect_once(NULL, PK_VEHICLE_PAIR_CANDIDATE, PK_PAIR_APPROVAL_MS);
    }
}

static void handle_ble_connected_event(pk_app_t *app, int slot, uint32_t conn_gen, pk_ms_t now)
{
    size_t i;
    bool sent_req = false;

    pk_session_set_conn_gen(&s_session, conn_gen);
    s_conn_ready_ms = now;
    if (port_bound_ble()) {
        port_bound_ble()->connected_slot = slot;
    }
    for (i = 0; i < 2; ++i) {
        active_wire_txn_t *txn = &s_wire_txns[i];
        if (!txn->active || !txn->waiting_connect ||
            txn->slot != slot || pk_time_due(now, txn->deadline)) {
            continue;
        }
        txn->waiting_connect = false;
        txn->conn_gen = conn_gen;
        if (txn->action == PK_ACTION_ADDKEY && txn->inner_len > 0) {
            pk_tx_outcome_t outcome = send_addkey_direct(txn);
            app->last_tx_outcome = outcome;
            if (port_bound_ble()) {
                port_bound_ble()->last_tx_outcome = outcome;
            }
            if (outcome == PK_TX_DELIVERED) {
                txn->addkey_transmitted = true;
                txn->last_approval_probe_ms = now;
                pk_session_note_request_ex(&s_session, txn->slot, conn_gen, 0u,
                                           txn->domain, txn->routing_addr,
                                           txn->uuid, false, now);
                sent_req = true;
            } else {
                txn->active = false;
            }
        } else if (txn->action == PK_ACTION_PAIR_VERIFY_VCSEC) {
            send_domain_session_request(slot, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
            sent_req = true;
        }
    }
    if (!sent_req && slot != PK_VEHICLE_PAIR_CANDIDATE) {
        pk_proto_key_slot_t *ks = port_slot_keys(slot);
        if (ks && ks->vcsec.valid) {
            /* Reuse the surviving VCSEC session without eagerly handshaking
             * Infotainment (Official A1: pure VCSEC ops stay on VCSEC). */
            start_background_status_probe(app, slot, conn_gen, ks, port_slot_vin(slot));
        } else {
            /* Handshake VCSEC only; Infotainment handshakes on demand. */
            request_session_if_idle(slot, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
        }
    }
    if (slot == app->vehicle && slot != PK_VEHICLE_NONE) {
        pk_app_set_connected(app, true);
    }
}

static void poll_pair_step3_status(const pk_app_t *app, pk_ms_t now)
{
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;

    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (app->page != PK_PAGE_PAIR || !pk_time_before(now, app->pair_deadline) ||
        conn_h == BLE_HS_CONN_HANDLE_NONE || tx_h == 0 ||
        active_slot != PK_VEHICLE_PAIR_CANDIDATE || !s_wire_txns[0].active) {
        return;
    }
    if (app->pair_step == 3 && !app->approved &&
        s_wire_txns[0].action == PK_ACTION_ADDKEY && s_wire_txns[0].addkey_transmitted &&
        pk_time_elapsed(now, s_wire_txns[0].last_approval_probe_ms, 2000u)) {
        s_wire_txns[0].last_approval_probe_ms = now;
        send_domain_session_request(PK_VEHICLE_PAIR_CANDIDATE, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
    } else if (app->pair_step == 4 && app->approved &&
               s_wire_txns[0].action == PK_ACTION_PAIR_VERIFY_VCSEC &&
               pk_time_elapsed(now, s_wire_txns[0].last_approval_probe_ms, 2000u)) {
        s_wire_txns[0].last_approval_probe_ms = now;
        if (!s_wire_txns[0].pair_vcsec_ok) {
            send_domain_session_request(PK_VEHICLE_PAIR_CANDIDATE, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
        } else if (!s_wire_txns[0].pair_infotain_ok) {
            send_domain_session_request(PK_VEHICLE_PAIR_CANDIDATE, PK_PROTO_DOMAIN_INFOTAINMENT);
        }
    }
}

static void poll_ble_rssi(pk_ms_t now)
{
    int8_t rssi = 0;
    uint16_t conn_h;
    uint16_t tx_h;

    radio_snapshot(&conn_h, &tx_h, NULL, NULL);
    if (conn_h == BLE_HS_CONN_HANDLE_NONE) {
        s_ble_rssi_valid = false;
        return;
    }
    if (tx_h == 0 || !pk_time_elapsed(now, s_ble_rssi_poll_ms, 1000u)) {
        return;
    }
    s_ble_rssi_poll_ms = now;
    if (ble_gap_conn_rssi(conn_h, &rssi) == 0) {
        s_ble_rssi = rssi;
        s_ble_rssi_valid = true;
    }
}

static void poll_selected_vehicle_scan(pk_app_t *app)
{
    /* When active saved vehicle changes (or on first boot with a saved car),
     * start a bounded background reconnect for the selected car if not in AP/Pair.
     */
    if (app->vehicle == s_last_observed_vehicle) {
        return;
    }
    s_last_observed_vehicle = app->vehicle;
    if (app->vehicle != PK_VEHICLE_NONE && !app->connected &&
        !app->ap && app->page != PK_PAGE_PAIR && !app->low_battery) {
        (void)pk_app_start_reconnect(app, false);
    }
}

static void poll_pair_ble_connect(const pk_app_t *app, pk_ms_t now)
{
    uint16_t conn_h;

    if (!app || app->page != PK_PAGE_PAIR || app->approved ||
        !pk_time_before(now, app->pair_deadline)) {
        return;
    }
    radio_snapshot(&conn_h, NULL, NULL, NULL);
    if (conn_h == BLE_HS_CONN_HANDLE_NONE && !ble_gap_disc_active() &&
        pk_time_elapsed(now, s_last_pair_scan_ms, 2000u)) {
        s_last_pair_scan_ms = now;
        (void)radio_connect_once(NULL, PK_VEHICLE_PAIR_CANDIDATE, PK_PAIR_APPROVAL_MS);
    }
}

void port_ble_init_and_attach(pk_app_t *app, pk_ble_adapter_t *ble)
{
    if (!s_gatt_tx_sem) {
        s_gatt_tx_sem = xSemaphoreCreateBinary();
        if (!s_gatt_tx_sem) {
            ESP_LOGE(TAG, "xSemaphoreCreateBinary failed: GATT TX completion channel unavailable");
        }
    }
    if (!s_ble_stack_ready) {
        esp_err_t err = nimble_port_init();
        if (err == ESP_OK) {
            pk_proto_set_entropy_source(PK_ENTROPY_SRC_BLE, true);
            ble_hs_cfg.sync_cb = on_ble_sync;
#if CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
            ble_svc_gap_init();
            (void)ble_svc_gap_device_name_set("PASSPORT-KEY");
#endif
            nimble_port_freertos_init(nimble_host_task);
        } else {
            ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        }
    }

    pk_gatt_tx_init(&s_gatt_tx, s_conn_gen, 23u);
    pk_session_init(&s_session);
    pk_session_set_conn_gen(&s_session, s_conn_gen);
    pk_ble_adapter_init(ble);
    ble->radio.write_once = radio_write_once;
    ble->radio.cancel = radio_cancel;
    ble->radio.connect_once = radio_connect_once;
    ble->radio.disconnect = radio_disconnect;
    ble->radio.has_session = radio_has_session;
    ble->radio.now_ms = radio_now_ms;
    ble->radio.get_mtu = radio_get_mtu;
    pk_ble_adapter_attach(app, ble);
}

void port_ble_deinit(void)
{
    uint16_t conn_h;
    uint32_t new_gen;

    if (!s_ble_stack_ready) {
        return;
    }
    (void)ble_gap_disc_cancel();
    portENTER_CRITICAL(&s_radio_mux);
    conn_h = s_conn_handle;
    s_conn_gen++;
    if (s_conn_gen == 0u) {
        s_conn_gen = 1u;
    }
    new_gen = s_conn_gen;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_active_conn_slot = PK_VEHICLE_NONE;
    s_target_slot = PK_VEHICLE_NONE;
    s_tx_val_handle = 0;
    s_rx_val_handle = 0;
    s_ble_params_applied = false;
    pk_gatt_tx_on_disconnect(&s_gatt_tx, new_gen);
    portEXIT_CRITICAL(&s_radio_mux);
    if (s_gatt_tx_sem) {
        xSemaphoreGive(s_gatt_tx_sem);
    }
    if (conn_h != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn_h, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (nimble_port_stop() == 0) {
        (void)nimble_port_deinit();
        /* NimBLE host task has fully stopped (stop barrier): safe to reclaim
         * any callback tokens whose procedures were still outstanding. */
        portENTER_CRITICAL(&s_radio_mux);
        memset(s_gatt_tok_pool, 0, sizeof(s_gatt_tok_pool));
        portEXIT_CRITICAL(&s_radio_mux);
    }
    pk_proto_set_entropy_source(PK_ENTROPY_SRC_BLE, false);
    s_ble_stack_ready = false;
    s_ble_rssi_valid = false;
    memset(s_wire_txns, 0, sizeof(s_wire_txns));
    pk_session_init(&s_session);
    pk_session_set_conn_gen(&s_session, new_gen);
}

void port_ble_stop_scan(void)
{
    if (!s_ble_stack_ready) {
        return;
    }
    (void)ble_gap_disc_cancel();
}

void port_ble_handle_event(pk_app_t *app, const port_event_t *ev, pk_ms_t now)
{
    pk_ble_adapter_t *ble = port_bound_ble();
    uint32_t cur_conn_gen = 0;

    radio_snapshot_ex(NULL, NULL, NULL, NULL, &cur_conn_gen);
    switch (ev->type) {
    case PORT_EV_BLE_CONNECTED:
        if (ev->conn_gen != cur_conn_gen) {
            break; /* stale connect event from a prior connection generation */
        }
        handle_ble_connected_event(app, ev->slot, ev->conn_gen, now);
        break;
    case PORT_EV_BLE_DISCONNECTED:
        pk_session_on_disconnect(&s_session, ev->slot);
        if (ev->conn_gen != cur_conn_gen) {
            break; /* newer connection already superseded this disconnect */
        }
        pk_session_set_conn_gen(&s_session, cur_conn_gen);
        memset(s_wire_txns, 0, sizeof(s_wire_txns));
        s_ble_rssi_valid = false;
        if (ble && ble->connected_slot == ev->slot) {
            ble->connected_slot = PK_VEHICLE_NONE;
        }
        if (ev->slot == app->vehicle && app->connected) {
            pk_app_set_connected(app, false);
        }
        break;
    case PORT_EV_BLE_RX_FRAME: {
        const uint8_t *rx = port_rx_buf_ptr(ev->u.rx_buf_idx);
        if (rx && ev->conn_gen == cur_conn_gen) {
            handle_rx_frame(app, ev->slot, ev->conn_gen, rx, ev->len);
        }
        port_rx_buf_release(ev->u.rx_buf_idx);
        break;
    }
    case PORT_EV_BLE_SLOTS_FULL:
        pk_app_ble_slots_full(app);
        break;
    default:
        break;
    }
}

static void expire_wire_txns(pk_app_t *app, pk_ms_t now)
{
    size_t i;

    for (i = 0; i < 2; ++i) {
        if (s_wire_txns[i].active &&
            pk_time_due(now, s_wire_txns[i].deadline)) {
            /* The adapter keeps its own in-flight slot; clear both or the
             * next send is rejected as BUSY until a user command cancels. */
            if (app && app->hal.ble && app->hal.ble->cancel) {
                (void)app->hal.ble->cancel(app->hal.ble_ctx,
                                           s_wire_txns[i].txn_id);
            }
            s_wire_txns[i].active = false;
        }
    }
}

static void poll_vcsec_reconcile(pk_app_t *app, pk_ms_t now)
{
    uint16_t conn_h;
    uint16_t tx_h;
    uint32_t conn_gen;
    int active_slot;
    pk_proto_key_slot_t *ks;
    const char *vin;

    if (!app || !app->connected || app->vehicle == PK_VEHICLE_NONE ||
        app->has_pending || app->busy || app->vcsec_status_valid ||
        app->vcsec_reconcile_attempts >= 2u || s_wire_txns[0].active) {
        return;
    }
    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (conn_h == BLE_HS_CONN_HANDLE_NONE || tx_h == 0 || active_slot != app->vehicle) {
        return;
    }
    conn_gen = s_session.conn_gen;
    ks = port_slot_keys(active_slot);
    vin = port_slot_vin(active_slot);
    if (!ks || !ks->vcsec.valid || !vin) {
        return;
    }
    if (app->vcsec_reconcile_next_ms == 0u) {
        app->vcsec_reconcile_next_ms = now + 1000u;
        return;
    }
    if (!pk_time_due(now, app->vcsec_reconcile_next_ms)) {
        return;
    }
    app->vcsec_reconcile_attempts++;
    app->vcsec_reconcile_next_ms = now + 2000u;
    start_background_status_probe(app, active_slot, conn_gen, ks, vin);
}

void port_ble_poll(pk_app_t *app, pk_ms_t now)
{
    expire_wire_txns(app, now);
    poll_pair_ble_connect(app, now);
    poll_pair_step3_status(app, now);
    poll_vcsec_reconcile(app, now);
    maybe_update_ble_conn_params(app);
    poll_ble_rssi(now);
    poll_selected_vehicle_scan(app);
}

bool port_ble_rssi(int8_t *out_rssi)
{
    if (!s_ble_rssi_valid) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = s_ble_rssi;
    }
    return true;
}

int port_ble_pair_state(const pk_app_t *app)
{
    uint16_t conn_h;
    uint16_t tx_h;
    int active_slot;

    if (!app || app->page != PK_PAGE_PAIR) {
        return 0;
    }
    if (s_wire_txns[0].active && s_wire_txns[0].action == PK_ACTION_ADDKEY &&
        s_wire_txns[0].addkey_transmitted) {
        return 2;
    }
    radio_snapshot(&conn_h, &tx_h, NULL, &active_slot);
    if (conn_h != BLE_HS_CONN_HANDLE_NONE && tx_h != 0 &&
        active_slot == PK_VEHICLE_PAIR_CANDIDATE) {
        return 1;
    }
    return 0;
}
