#include "pk_app.h"
#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_copy.h"
#include "pk_feedback.h"
#include "pk_plan.h"
#include "pk_power.h"
#include "pk_risk.h"
#include "pk_session.h"
#include "pk_btn_adapter.h"
#include "pk_protocol.h"
#include "pk_store.h"

#include <stdio.h>
#include <string.h>

/* Internal helper, compiled from components/tesla_key/src/pk_pb.c. Declared here
 * to pin the exact AAD bytes for the official protocol.md metadata vector. */
extern bool pk_proto_compute_cmd_metadata_hash(pk_proto_domain_t domain, const char *vin,
                                               const uint8_t epoch[16], uint32_t expires_at,
                                               uint32_t counter, uint32_t flags,
                                               uint8_t aad_out[32]);

static int g_fail;

static void expect(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        int hi = hex_nib(hex[0]);
        int lo = hex_nib(hex[1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static const char *VIN_A = "5YJ3E1EA1PF000001";

static int add_and_select(pk_app_t *app, const char *vin)
{
    int slot;

    if (!pk_store_add_verified(&app->store, vin, PK_STORE_CRASH_NONE)) {
        return -1;
    }
    slot = pk_store_find_vin(&app->store, vin);
    if (slot < 0) {
        return -1;
    }
    if (!pk_app_select_vehicle(app, slot)) {
        return -1;
    }
    return slot;
}

static pk_err_t fake_write(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    return PK_OK;
}

static pk_err_t fake_connect(void *ctx, int slot, pk_ms_t budget)
{
    (void)ctx;
    (void)slot;
    (void)budget;
    return PK_OK;
}

static pk_err_t fake_disconnect(void *ctx, int slot)
{
    (void)ctx;
    (void)slot;
    return PK_OK;
}

static pk_err_t fake_cancel(void *ctx, pk_id_t txn)
{
    (void)ctx;
    (void)txn;
    return PK_OK;
}

static bool fake_has_session(void *ctx, int slot, pk_domain_t domain);

static void attach_radio(pk_app_t *app, pk_ble_adapter_t *ad)
{
    pk_ble_adapter_init(ad);
    ad->radio.write_once = fake_write;
    ad->radio.connect_once = fake_connect;
    ad->radio.disconnect = fake_disconnect;
    ad->radio.cancel = fake_cancel;
    ad->radio.has_session = fake_has_session;
    pk_ble_adapter_attach(app, ad);
}

static void grant_park(pk_app_t *app)
{
    memset(&app->grant, 0, sizeof(app->grant));
    app->has_grant = true;
    app->grant.scope = pk_app_scope(app);
    app->grant.until = app->now + PK_PARK_LEASE_MS;
    app->grant.received = app->now;
    app->grant.proof = true;
    app->grant.parked = true;
    app->grant.speed_present = true;
}

static void test_unwired_not_success(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_err_t err;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    expect(add_and_select(&app, VIN_A) >= 0, "add current car");
    pk_app_set_connected(&app, true);
    pk_ble_adapter_init(&ad);
    pk_ble_adapter_attach(&app, &ad);
    err = pk_ble_send_once(&app, PK_DOMAIN_VCSEC, PK_ACTION_LOCK, 5000);
    expect(err == PK_ERR_NOT_WIRED, "unwired send is NOT_WIRED");
}

static void test_p16_reboot_releases_candidate(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_err_t rc;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    expect(add_and_select(&app, VIN_A) >= 0, "add current car");
    pk_app_set_connected(&app, true);
    attach_radio(&app, &ad);
    rc = app.hal.ble->connect_once(app.hal.ble_ctx, PK_VEHICLE_PAIR_CANDIDATE, 1000);
    expect(rc == PK_OK, "candidate connect");
    app.pair_radio_held = true;
    app.pair_scope.vehicle = PK_VEHICLE_PAIR_CANDIDATE;
    pk_app_reboot(&app);
    expect(app.pair_radio_held == false, "P1-6 pair_radio_held cleared");
    expect(ad.connected_slot != PK_VEHICLE_PAIR_CANDIDATE,
           "P1-6 adapter not left on candidate");
    expect(app.connected == false, "reboot leaves app unconnected");
}

static void test_connect_none_invalid(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(app.hal.ble->connect_once(app.hal.ble_ctx, PK_VEHICLE_NONE, 1000)
               == PK_ERR_INVALID,
           "connect_once(NONE) is invalid");
}

static void test_protocol_crypto_and_wire(void)
{
    char ble_name[PK_PROTO_BLE_NAME_BUF];
    uint8_t priv_a[32], pub_a[65];
    uint8_t priv_b[32], pub_b[65];
    uint8_t key_ab[16], key_ba[16];
    uint8_t seed_a[32], seed_b[32];
    uint8_t sess_info[128];
    uint8_t challenge[16];
    uint8_t tag[32];
    uint8_t routing_addr[16];
    uint8_t uuid[16];
    uint8_t nonce[12];
    uint8_t req_id[17];
    uint8_t inner[64];
    uint8_t routable_buf[512];
    uint8_t framed_buf[520];
    size_t inner_len, routable_len, framed_len;
    pk_proto_session_t client_sess;
    pk_rx_framer_t framer;
    pk_proto_routable_t parsed_msg;
    pk_proto_vcsec_resp_t vcsec_resp;
    pk_proto_carserver_resp_t cs_resp;
    int i;

    /* 1. Official protocol.md test vector: VIN 5YJS0000000000000 -> S1a87a5a75f3df858C */
    expect(pk_proto_vin_to_ble_name("5YJS0000000000000", ble_name), "vin_to_ble_name ok");
    expect(strcmp(ble_name, "S1a87a5a75f3df858C") == 0, "vin_to_ble_name matches official vector");
    expect(pk_proto_vin_to_ble_name("5YJSA1E26HF000337", ble_name) &&
           strcmp(ble_name, "S52722d5c20e5e4a2C") == 0, "vin_to_ble_name second vector");

    /* 2. P-256 keygen & ECDH symmetry */
    for (i = 0; i < 32; i++) {
        seed_a[i] = (uint8_t)(i + 1);
        seed_b[i] = (uint8_t)(0xA0 + i);
    }
    expect(pk_proto_p256_keygen(priv_a, pub_a, seed_a), "p256 keygen A");
    expect(pk_proto_p256_keygen(priv_b, pub_b, seed_b), "p256 keygen B");
    expect(pk_proto_ecdh_derive_session_key(priv_a, pub_b, key_ab), "ecdh A->B");
    expect(pk_proto_ecdh_derive_session_key(priv_b, pub_a, key_ba), "ecdh B->A");
    expect(memcmp(key_ab, key_ba, 16) == 0, "ecdh shared session key matches");

    /* 3. SessionInfo HMAC verification & session establishment */
    /* Build a synthetic SessionInfo protobuf from vehicle B:
     * field 1 (counter=10): 0x08 0x0A
     * field 2 (publicKey=65B): 0x12 0x41 <pub_b>
     * field 3 (epoch=16B): 0x1A 0x10 <16B>
     * field 4 (clock_time=1000 fixed32): 0x25 0xE8 0x03 0x00 0x00
     */
    {
        size_t pos = 0;
        sess_info[pos++] = 0x08; sess_info[pos++] = 10;
        sess_info[pos++] = 0x12; sess_info[pos++] = 65;
        memcpy(sess_info + pos, pub_b, 65); pos += 65;
        sess_info[pos++] = 0x1A; sess_info[pos++] = 16;
        for (i = 0; i < 16; i++) { sess_info[pos++] = (uint8_t)(0x30 + i); }
        sess_info[pos++] = 0x25;
        sess_info[pos++] = 0xE8; sess_info[pos++] = 0x03; sess_info[pos++] = 0x00; sess_info[pos++] = 0x00;

        for (i = 0; i < 16; i++) {
            challenge[i] = (uint8_t)(0x50 + i);
            routing_addr[i] = (uint8_t)(0x70 + i);
            uuid[i] = (uint8_t)(0x90 + i);
        }
        for (i = 0; i < 12; i++) {
            nonce[i] = (uint8_t)(0xB0 + i);
        }

        pk_proto_session_apply_t apply = {
            .info = {
                .vin              = VIN_A,
                .challenge        = challenge,
                .challenge_len    = 16u,
                .encoded_info     = sess_info,
                .encoded_info_len = pos,
            },
            .privkey = priv_a,
            .tag     = tag,
            .tag_len = 32u,
            .now_ms  = 1000,
        };
        expect(pk_proto_session_info_tag_compute(key_ba, &apply.info, tag),
               "compute session_info HMAC tag");
        memset(&client_sess, 0, sizeof(client_sess));
        expect(pk_proto_session_apply_info(&client_sess, &apply),
               "apply verified SessionInfo");
        expect(client_sess.valid && client_sess.counter == 10 && client_sess.clock_time == 1000,
               "session state populated");

        /* Tamper with tag -> must reject */
        tag[0] ^= 0xFF;
        expect(!pk_proto_session_apply_info(&client_sess, &apply),
               "tampered SessionInfo tag rejected");
    }

    /* 4. Signed RoutableMessage build + 2-byte BE framing + RxFramer + parse */
    inner_len = pk_proto_encode_action_payload(PK_ACTION_LOCK, NULL, pub_a, inner, sizeof(inner));
    expect(inner_len > 0, "encode LOCK payload");
    {
        pk_proto_signed_req_t req = {
            .route         = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, routing_addr, uuid },
            .vin           = VIN_A,
            .client_pubkey = pub_a,
            .inner_payload = inner,
            .inner_len     = inner_len,
            .nonce         = nonce,
            .now_ms        = 2000,
            .ttl_seconds   = 5u,
        };
        routable_len = pk_proto_build_signed_routable(
            &req, &client_sess, req_id, routable_buf, sizeof(routable_buf));
    }
    expect(routable_len > 0, "build signed RoutableMessage");
    expect(client_sess.counter == 11, "counter incremented to 11");
    expect(req_id[0] == 5, "req_id prefix is SIGNATURE_TYPE_AES_GCM_PERSONALIZED(5)");

    framed_len = pk_proto_frame_wrap(routable_buf, routable_len, framed_buf, sizeof(framed_buf));
    expect(framed_len == routable_len + 2, "frame wrap adds 2-byte BE header");

    pk_rx_framer_init(&framer);
    expect(pk_rx_framer_feed(&framer, framed_buf, 5, 100) == 0, "framer partial chunk 1");
    expect(pk_rx_framer_feed(&framer, framed_buf + 5, framed_len - 5, 200) == 1, "framer complete");
    expect(framer.expected_len == routable_len, "framer length matches");
    expect(pk_proto_parse_routable(framer.buf, framer.expected_len, &parsed_msg), "parse routable");
    expect(parsed_msg.to_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY, "routable to_domain VCSEC");
    expect(parsed_msg.has_from_routing_addr &&
           memcmp(parsed_msg.from_routing_addr, routing_addr, 16) == 0,
           "routable from_routing_addr preserved");
    expect(parsed_msg.flags == PK_PROTO_FLAG_ENCRYPT_RESPONSE,
           "signed request sets FLAG_ENCRYPT_RESPONSE(2)");
    {
        /* TLV layout and tag order from official protocol.md "Authorizing commands"
         * (INFOTAINMENT, epoch 4c46...dde6, expires_at 2655, counter 7, flags 2),
         * using the repository's synthetic VIN_A. The expected SHA-256 was computed
         * independently from the documented byte layout. */
        static const uint8_t epoch[16] = {
            0x4c, 0x46, 0x3f, 0x9c, 0xc0, 0xd3, 0xd2, 0x69,
            0x06, 0xe9, 0x82, 0xed, 0x22, 0x4a, 0xdd, 0xe6,
        };
        static const uint8_t expected_aad[32] = {
            0x81, 0xc9, 0xfe, 0x81, 0xe0, 0x64, 0x22, 0xd4,
            0xf9, 0x63, 0xe6, 0x9f, 0xf7, 0xa5, 0xf0, 0xc0,
            0x4b, 0xa6, 0x51, 0xc4, 0xb9, 0x08, 0xa1, 0xbd,
            0xb0, 0xa2, 0xa4, 0xac, 0x4c, 0x53, 0x73, 0x3d,
        };
        uint8_t aad[32];

        expect(pk_proto_compute_cmd_metadata_hash(PK_PROTO_DOMAIN_INFOTAINMENT,
                                                  VIN_A, epoch, 2655u, 7u,
                                                  PK_PROTO_FLAG_ENCRYPT_RESPONSE, aad),
               "compute command metadata hash with flags");
        expect(memcmp(aad, expected_aad, sizeof(aad)) == 0,
               "metadata AAD matches documented TLV layout");
    }

    /* 5. VCSEC VehicleStatus & CarServer VehicleData parsers */
    {
        /* FromVCSECMessage { vehicleStatus(1) { closureStatuses(1) { frontDriverDoor(1)=OPEN(1) }, vehicleLockState(2)=LOCKED(1) } } */
        const uint8_t vcsec_bin[] = { 0x0A, 0x06, 0x0A, 0x02, 0x08, 0x01, 0x10, 0x01 };
        expect(pk_proto_parse_vcsec_response(vcsec_bin, sizeof(vcsec_bin), &vcsec_resp),
               "parse vcsec VehicleStatus");
        expect(vcsec_resp.has_vehicle_status && vcsec_resp.has_lock_state &&
               vcsec_resp.lock_state == PK_LOCK_READ_LOCKED &&
               vcsec_resp.has_closure_statuses &&
               vcsec_resp.closures[PK_CLOSURE_FRONT_LEFT] == PK_CLOSURE_OPEN &&
               vcsec_resp.closures[PK_CLOSURE_FRONT_RIGHT] == PK_CLOSURE_CLOSED,
               "vcsec VehicleStatus fields decoded accurately");
    }
    {
        /* CarServer.Response { actionStatus(1) { result(1)=OK(0) },
         *   vehicleData(2) { drive_state(5) { shift_state(1) { P(2)={} }, speed(102)=0 } } }
         * Tag for field 102 varint: (102 << 3) | 0 = 816 = 0xB0 0x06
         */
        const uint8_t cs_bin[] = {
            0x0A, 0x02, 0x08, 0x00,
            0x12, 0x09, 0x2A, 0x07, 0x0A, 0x02, 0x12, 0x00, 0xB0, 0x06, 0x00
        };
        expect(pk_proto_parse_carserver_response(cs_bin, sizeof(cs_bin), &cs_resp),
               "parse CarServer Response");
        expect(cs_resp.has_action_status && cs_resp.result == 0 &&
               cs_resp.has_vehicle_data && cs_resp.has_drive_state &&
               cs_resp.shift == PK_SHIFT_P && cs_resp.speed_present && cs_resp.speed_mph == 0,
               "CarServer DriveState P and speed=0 decoded accurately");
    }
}

static void test_workflow_closure(void)
{
    pk_app_t app;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    expect(add_and_select(&app, VIN_A) >= 0, "add car A");
    expect(!app.connected && app.page == PK_PAGE_MAIN, "starts offline on MAIN");

    /* Double-click ● on offline MAIN must still open PK_PAGE_CARS */
    (void)pk_app_button_edge(&app, 100, PK_KEY_DOT, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 150, PK_KEY_DOT, PK_EDGE_RELEASE);
    (void)pk_app_button_edge(&app, 250, PK_KEY_DOT, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 300, PK_KEY_DOT, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_CARS, "offline double-click opens CARS list");

    /* Navigate to PK_LIST_ADD while offline and press ● -> enters PK_PAGE_VIN */
    app.car_focus = PK_LIST_ADD;
    (void)pk_app_button_edge(&app, 700, PK_KEY_DOT, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 750, PK_KEY_DOT, PK_EDGE_RELEASE);
    (void)pk_app_tick(&app, 750 + PK_DOUBLE_MS + 10);
    expect(app.page == PK_PAGE_VIN && app.ap, "offline select ADD enters VIN page");

    /* Press ▼ on PK_PAGE_VIN -> cancels AP and returns to PK_PAGE_CARS */
    (void)pk_app_button_edge(&app, 750 + PK_DOUBLE_MS + 100, PK_KEY_DOWN, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 750 + PK_DOUBLE_MS + 150, PK_KEY_DOWN, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_CARS && !app.ap, "down key on VIN cancels AP and returns to CARS");
}

static void test_nocar_double_click_via_adc_ladder(void)
{
    pk_app_t app;
    pk_btn_adapter_t btn;
    pk_ms_t t;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    pk_btn_adapter_init(&btn, &app);
    expect(app.vehicle == PK_VEHICLE_NONE && app.page == PK_PAGE_MAIN, "starts 0-car on MAIN");

    /* Simulate realistic human double-click on middle ● (303 mV) with 100 ms press and 140 ms gap
     * under 40 ms CONFIRM / 120 ms HOLD voltage lock. */
    for (t = 100; t <= 600; t += 10) {
        int mv = 3300;
        if ((t >= 100 && t < 200) || (t >= 340 && t < 440)) {
            mv = 303;
        }
        (void)pk_btn_adapter_feed_mv(&btn, t, mv, true);
        (void)pk_app_tick(&app, t);
    }
    expect(app.page == PK_PAGE_CARS, "0-car double-click via ADC ladder enters CARS");
}

static void test_batch_1_to_3_regressions(void)
{
    pk_app_t app;
    uint8_t pub[65];
    uint8_t wire[128];
    size_t wire_len;

    /* 1. pk_proto_encode_vcsec_add_key produces ToVCSECMessage (outer field 1 = unsignedMessage),
     *    NOT wrapped in UniversalMessage.RoutableMessage.
     */
    memset(pub, 0x22, sizeof(pub));
    pub[0] = 0x04;
    wire_len = pk_proto_encode_vcsec_add_key(pub, PK_PROTO_ROLE_DRIVER,
                                             PK_PROTO_KEY_FORM_UNKNOWN, wire, sizeof(wire));
    expect(wire_len > 70 && wire[0] == 0x0A, "AddKey starts with ToVCSECMessage.unsignedMessage (0x0A)");

    /* 2. pair_step == 2 must not deadlock when pair_unknown_form_supported == PK_CAP_UNKNOWN (c3f36cc). */
    {
        pk_ble_adapter_t ad;
        pk_app_init(&app, PK_VEHICLE_NONE, false);
        attach_radio(&app, &ad);
        app.ap = true;
        expect(pk_app_submit_vin(&app, VIN_A), "submit_vin succeeds");
        expect(pk_app_pair_start(&app, VIN_A, true, true), "pair_start enters step 1");
    }
    expect(app.pair_step == 1, "pair_step starts at 1");
    /* Step 1 -> Step 2 via DOT click */
    (void)pk_app_button_edge(&app, 100, PK_KEY_DOT, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 150, PK_KEY_DOT, PK_EDGE_RELEASE);
    (void)pk_app_tick(&app, 150 + PK_DOUBLE_MS + 10);
    expect(app.pair_step == 2, "DOT click advances step 1 -> step 2");
    expect(app.caps.pair_unknown_form_supported == PK_CAP_UNKNOWN, "unknown form starts PK_CAP_UNKNOWN");
    /* Step 2 -> Step 3 via DOT click even when PK_CAP_UNKNOWN */
    (void)pk_app_button_edge(&app, 600, PK_KEY_DOT, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 650, PK_KEY_DOT, PK_EDGE_RELEASE);
    (void)pk_app_tick(&app, 650 + PK_DOUBLE_MS + 10);
    expect(app.pair_step == 3, "pair_step == 2 advances to step 3 when unknown_form == PK_CAP_UNKNOWN");

    /* 3. Struct-parameter APIs (pk_app_lock_packet, pk_app_status_field, pk_app_pair_verify) */
    {
        pk_lock_packet_t lpkt = {
            .request_id = 0,
            .authenticated = false,
            .correlated = false,
            .adapter_verified = false,
            .lock = PK_LOCK_READ_LOCKED,
            .closures = NULL,
            .closures_complete = false,
            .complete = true,
        };
        pk_scope_t sc = pk_app_scope(&app);
        pk_status_input_t sin = {
            .request_id = 0,
            .scope = &sc,
            .name = "soc",
            .value = 80,
            .present = true,
            .sample_present = true,
            .sampled = 1100,
            .unit_verified = true,
            .clock_verified = false,
        };
        pk_pair_verify_t pv = {
            .request_id = 0,
            .vcsec_authenticated = true,
            .infotainment_authenticated = true,
            .key_matches = true,
            .role_driver = true,
            .identity_binding_verified = true,
        };
        expect(!pk_app_lock_packet(&app, &lpkt), "lock_packet rejects unwired/no-vehicle safely");
        expect(!pk_app_status_field(&app, &sin), "status_field rejects when not on STATUS page");
        expect(!pk_app_pair_verify(&app, &pv), "pair_verify rejects before step 4");
    }
}

static void test_deep_sleep_and_down_shortcut(void)
{
    pk_app_t app;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    expect(add_and_select(&app, VIN_A) >= 0, "add car A for deep sleep & shortcut");

    /* 1. Deep sleep gating */
    app.power = PK_POWER_COMFORT;
    app.awake = false;
    expect(!pk_app_should_deep_sleep(&app), "comfort mode never deep sleeps");

    app.power = PK_POWER_ENDURANCE;
    app.awake = true;
    expect(!pk_app_should_deep_sleep(&app), "awake screen vetoes deep sleep");

    app.awake = false;
    expect(pk_app_should_deep_sleep(&app), "endurance + screen off allows deep sleep");

    app.connected = true;
    app.connected = false;

    app.ap = true;
    expect(!pk_app_should_deep_sleep(&app), "active AP vetoes deep sleep");
    app.ap = false;

    app.reconnect.active = true;
    expect(!pk_app_should_deep_sleep(&app), "active reconnect vetoes deep sleep");
    app.reconnect.active = false;

    /* 2. ▼ double-click MAIN <-> STATUS shortcut */
    app.awake = true;
    app.page = PK_PAGE_MAIN;
    (void)pk_app_button_edge(&app, 100, PK_KEY_DOWN, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 150, PK_KEY_DOWN, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_MAIN, "first ▼ tap stays on MAIN");
    (void)pk_app_button_edge(&app, 250, PK_KEY_DOWN, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 300, PK_KEY_DOWN, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_MAIN,
           "▼ no longer doubles as the status shortcut");

    (void)pk_app_button_edge(&app, 500, PK_KEY_DOWN, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 550, PK_KEY_DOWN, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_MAIN, "▼ keeps the page unchanged");
}

static void test_power_role_arbiter(void)
{
    pk_app_t app;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    app.awake = true;
    app.power = PK_POWER_COMFORT;
    expect(pk_power_role(&app) == PK_POWER_ROLE_ACTIVE, "power: awake is ACTIVE");
    expect(pk_power_screen_timeout(&app) == PK_SCREEN_MS, "power: active screen timeout");
    expect(pk_power_backlight_percent(&app) == app.brightness,
           "power: active uses user brightness");
    expect(!pk_power_should_deep_sleep(&app), "power: active never sleeps");

    app.awake = false;
    expect(pk_power_role(&app) == PK_POWER_ROLE_CONNECTED_IDLE,
           "power: comfort screen-off is CONNECTED_IDLE");
    expect(!pk_power_should_deep_sleep(&app),
           "power: connected idle keeps the MCU awake in comfort");

    app.power = PK_POWER_ENDURANCE;
    expect(pk_power_role(&app) == PK_POWER_ROLE_POCKET,
           "power: endurance screen-off is POCKET");
    expect(pk_power_should_deep_sleep(&app), "power: pocket may deep sleep");
    expect(pk_power_policy(PK_POWER_ROLE_POCKET)->allow_deep_sleep,
           "power: pocket policy allows deep sleep");

    app.reconnect.active = true;
    app.power = PK_POWER_COMFORT;
    expect(pk_power_role(&app) == PK_POWER_ROLE_SCAN, "power: reconnect is SCAN");
    expect(!pk_power_should_deep_sleep(&app), "power: scan vetoes deep sleep");
    app.reconnect.active = false;

    app.moving = true;
    app.power = PK_POWER_ENDURANCE;
    expect(pk_power_role(&app) == PK_POWER_ROLE_DRIVE_KEEPALIVE,
           "power: moving is DRIVE_KEEPALIVE");
    expect(!pk_power_should_deep_sleep(&app), "power: drive vetoes deep sleep");
    app.moving = false;

    app.connected = false;

    app.desktop = true;
    app.power = PK_POWER_ENDURANCE;
    expect(pk_power_role(&app) == PK_POWER_ROLE_DESKTOP,
           "power: USB host is DESKTOP");
    expect(pk_power_screen_timeout(&app) == PK_SCREEN_MS,
           "power: desktop keeps full screen timeout");
    expect(!pk_power_should_deep_sleep(&app), "power: desktop vetoes deep sleep");
    app.desktop = false;

    app.ap = true;
    expect(pk_power_role(&app) == PK_POWER_ROLE_DESKTOP, "power: AP is DESKTOP");
    expect(!pk_power_should_deep_sleep(&app), "power: AP vetoes deep sleep");
    app.ap = false;

    app.low_battery = true;
    app.awake = false;
    app.power = PK_POWER_COMFORT;
    expect(pk_power_role(&app) == PK_POWER_ROLE_LOW_BATT,
           "power: low battery is its own role");
    expect(pk_power_screen_timeout(&app) == PK_LOW_BATT_SCREEN_MS,
           "power: low battery short screen");
    expect(pk_power_backlight_percent(&app) == PK_BRIGHTNESS_MIN,
           "power: low battery min backlight");
    expect(pk_power_should_deep_sleep(&app),
           "power: low battery overrides comfort sleep veto");
    app.desktop = true;
    expect(!pk_power_should_deep_sleep(&app),
           "power: desktop still vetoes sleep under low battery");
    expect(pk_power_screen_timeout(&app) == PK_SCREEN_MS &&
           pk_power_backlight_percent(&app) == app.brightness,
           "power: desktop keeps full display under low battery");
    app.desktop = false;

    app.moving = true;
    expect(pk_power_role(&app) == PK_POWER_ROLE_LOW_BATT,
           "power: low battery outranks drive role");
    expect(!pk_power_should_deep_sleep(&app),
           "power: keepalive still vetoes sleep under low battery");
    expect(pk_power_screen_timeout(&app) == PK_SCREEN_MS,
           "power: drive keeps full screen timeout under low battery");
    expect(pk_power_backlight_percent(&app) == app.brightness,
           "power: drive keeps readable backlight under low battery");
    app.moving = false;

    app.lock_alert_active = true;
    expect(pk_power_backlight_percent(&app) == 100u,
           "power: lock alert forces full backlight");
    app.lock_alert_active = false;

    expect(pk_power_policy(PK_POWER_ROLE_COUNT) == NULL,
           "power: invalid role has no policy");
    expect(pk_power_policy(PK_POWER_ROLE_LOW_BATT)->allow_deep_sleep &&
           pk_power_policy(PK_POWER_ROLE_LOW_BATT)->min_backlight,
           "power: low battery policy sleeps after dimming");
}

static void test_power_low_battery_sequence(void)
{
    pk_app_t app;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    app.ap = true;
    app.reconnect.active = true;
    app.connected = true;
    app.awake = false;

    pk_app_battery(&app, true, 4, true);
    expect(app.low_battery, "low battery: state set");
    expect(!app.ap, "low battery: AP closed first");
    expect(!app.reconnect.active, "low battery: reconnect canceled");
    expect(app.message == PK_MSG_LOW_BATT, "low battery: user message");
    expect(pk_power_role(&app) == PK_POWER_ROLE_LOW_BATT,
           "low battery: role selected");
    expect(pk_power_backlight_percent(&app) == PK_BRIGHTNESS_MIN,
           "low battery: backlight floor");
    expect(pk_power_screen_timeout(&app) == PK_LOW_BATT_SCREEN_MS,
           "low battery: short screen timeout");
    expect(pk_power_should_deep_sleep(&app),
           "low battery: may deep sleep");
}

static void test_power_simulator(void)
{
    pk_power_estimate_t est;
    pk_power_phase_t one_hour_active[] = {
        { PK_POWER_ROLE_ACTIVE, 3600000u }
    };

    expect(pk_power_simulate(one_hour_active, 1u, 500u, &est),
           "sim: one-phase scenario runs");
    expect(est.avg_ua == 25000u, "sim: average current");
    expect(est.days_x100 == 83u, "sim: 500 mAh at 25 mA is about 0.83 day");

    expect(pk_power_common_car_estimate(500u, &est),
           "sim: common car scenario runs");
    expect(est.days_x100 >= 700u,
           "sim: common-car assumption reaches 7 days at 500 mAh (model)");
    expect(pk_power_role_current_ua(PK_POWER_ROLE_POCKET) <
           pk_power_role_current_ua(PK_POWER_ROLE_CONNECTED_IDLE),
           "sim: pocket is cheaper than connected idle");

    expect(!pk_power_simulate(NULL, 0u, 500u, &est),
           "sim: empty scenario rejected");
    expect(!pk_power_simulate(one_hour_active, 1u, 0u, &est),
           "sim: zero capacity rejected");
}

static void test_four_pillar_closures(void)
{
    pk_app_t app;
    pk_pair_verify_t pv = {
        .vcsec_authenticated = true,
        .infotainment_authenticated = true,
        .key_matches = true,
        .role_driver = true,
        .identity_binding_verified = true,
    };

    /* Pillar 1: first paired vehicle activates slot 0; second does not steal focus */
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    expect(pk_app_enter_vin(&app), "enter vin for first car");
    expect(pk_app_submit_vin(&app, VIN_A), "submit VIN_A");
    expect(pk_app_pair_start(&app, VIN_A, true, true), "pair_start first car");
    expect(pk_app_pair_next(&app), "step 1 -> 2");
    expect(pk_app_pair_next(&app), "step 2 -> 3");
    expect(pk_app_pair_approve(&app, true, app.pair_id, true), "approve first car");
    expect(pk_app_pair_next(&app), "step 3 -> 4");
    pv.request_id = app.pair_id;
    expect(pk_app_pair_verify(&app, &pv), "verify first car");
    expect(app.vehicle == 0 && app.paired, "first paired car activates slot 0");

    /* Pillar 2: background vs manual reconnect page behavior */
    app.page = PK_PAGE_MAIN;
    app.connected = false;
    expect(pk_app_start_reconnect(&app, false), "background reconnect starts");
    expect(app.page == PK_PAGE_MAIN, "background reconnect keeps MAIN page");
    pk_app_cancel_reconnect(&app);
    expect(pk_app_start_reconnect(&app, true), "manual reconnect starts");
    expect(app.page == PK_PAGE_OFFLINE, "manual reconnect enters OFFLINE page");
    pk_app_cancel_reconnect(&app);

    /* Pillar 3: status timeout stays on STATUS, and connected short-click ● refreshes */
    app.connected = true;
    app.page = PK_PAGE_MAIN;
    pk_app_open_status(&app, PK_ORIGIN_DRIVING);
    expect(app.page == PK_PAGE_STATUS && app.has_pending, "status opened with pending read");
    (void)pk_app_tick(&app, app.pending.deadline + 10);
    expect(app.page == PK_PAGE_STATUS && !app.has_pending &&
               app.message == PK_MSG_STATUS_TIMEOUT,
           "status timeout stays on PK_PAGE_STATUS with timeout msg");
    {
        pk_ms_t t0 = app.now + 100;
        (void)pk_app_button_edge(&app, t0, PK_KEY_DOT, PK_EDGE_PRESS);
        (void)pk_app_button_edge(&app, t0 + 50, PK_KEY_DOT, PK_EDGE_RELEASE);
        (void)pk_app_tick(&app, t0 + 50 + PK_DOUBLE_MS + 10);
    }
    expect(app.page == PK_PAGE_STATUS && app.has_pending &&
               strcmp(app.status_phase, "loading") == 0,
           "short-click ● on connected STATUS refreshes status read");
}

static void test_stable_branch_five_pillars(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_closure_t closures[PK_CLOSURE_COUNT];
    int i;

    /* 1. VCSEC native direct unlock without Infotainment DriveState 2s lease */
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "add VIN_A for native unlock");
    pk_app_set_connected(&app, true);
    app.now = 1000;
    expect(!app.has_grant, "no Infotainment grant initially");
    pk_app_vcsec_status(&app, true, PK_LOCK_READ_LOCKED);
    expect(pk_app_can_vcsec_unlock(&app),
           "explicit unlock is allowed when motion is unknown");
    expect(pk_app_primary(&app) == PK_ACTION_UNLOCK, "primary action becomes UNLOCK");
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, 1100), "commit native UNLOCK without Infotainment wake");
    expect(app.has_pending && app.pending.action == PK_ACTION_UNLOCK, "UNLOCK pending");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "UNLOCK accepted triggers VCSEC readback verify");
    expect(app.has_pending && app.pending.kind == PK_PENDING_UNLOCK_VERIFY,
           "pending UNLOCK_VERIFY");
    pk_app_vcsec_status(&app, false, PK_LOCK_READ_UNLOCKED);
    expect(!app.has_pending && app.page == PK_PAGE_MAIN &&
               app.message == PK_MSG_CMD_ACCEPTED,
           "VCSEC readback confirms unlocked inline");

    /* 3. Low-battery (<=5%) stops background auto-reconnect/AP, allows emergency UNLOCK/LOCK & fast sleep */
    pk_app_battery(&app, true, 4, true);
    expect(!pk_app_enter_vin(&app), "low battery blocks SoftAP VIN portal");
    pk_app_set_connected(&app, false);
    expect(!app.reconnect.active, "low battery suppresses background auto-reconnect");
    expect(pk_app_start_reconnect(&app, true), "low battery allows manual emergency reconnect");
    pk_app_set_connected(&app, true);
    pk_app_vcsec_status(&app, true, PK_LOCK_READ_LOCKED);
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, 2000), "low battery allows emergency UNLOCK");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true), "emergency UNLOCK accepted");
    pk_app_vcsec_status(&app, false, PK_LOCK_READ_UNLOCKED);
    (void)pk_app_tick(&app, app.last_activity + PK_LOW_BATT_SCREEN_MS + 10);
    expect(!app.awake, "low battery sleeps rapidly after 5s");
    pk_app_battery(&app, true, 80, false);

    /* 4a. A dropped LOCK is unknown. Reconnect must not send it again. */
    app.awake = true;
    app.last_activity = app.now;
    pk_app_set_connected(&app, true);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "commit LOCK");
    pk_app_set_connected(&app, false);
    expect(!app.has_pending && app.message == PK_MSG_LOCK_UNCONFIRMED,
           "LOCK drop ends unconfirmed and does not start a relay");
    pk_app_set_connected(&app, true);
    expect(!app.has_pending && app.message == PK_MSG_RECONNECTED,
           "reconnect does not commit a second LOCK");
    pk_app_exit_wait(&app);

    /* 4b. Unclosed closure triggers lock_alert_active & stays awake */
    pk_app_set_connected(&app, true);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "commit LOCK for closure alert test");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true), "start lock verify");
    for (i = 0; i < PK_CLOSURE_COUNT; i++) {
        closures[i] = PK_CLOSURE_CLOSED;
    }
    closures[PK_CLOSURE_FRONT_LEFT] = PK_CLOSURE_OPEN;
    {
        pk_lock_packet_t pkt = {
            .request_id = app.pending.id,
            .authenticated = true,
            .correlated = true,
            .adapter_verified = true,
            .lock = PK_LOCK_READ_LOCKED,
            .closures = closures,
            .closures_complete = true,
            .complete = true,
        };
        expect(pk_app_lock_packet(&app, &pkt), "lock verify packet processed");
    }
    expect(app.lock_alert_active && app.awake && strstr(app.message_extra, "前左未关") != NULL,
           "unclosed front-left door activates full-brightness red alert");
    (void)pk_app_tick(&app, app.last_activity + PK_LOW_BATT_SCREEN_MS + 10);
    expect(app.awake && app.lock_alert_active, "screen stays awake during alert notice window");
    pk_app_exit_wait(&app);

    /* 5. BLE slots full fast-fail + single-car skips redundant car selection */
    pk_app_ble_slots_full(&app);
    expect(app.message == PK_MSG_BLE_SLOTS_FULL && !app.reconnect.active,
           "NONCONN slots-full stops scan and sets prompt");
    app.page = PK_PAGE_MAIN;
    app.connected = true;
    {
        pk_ms_t t0 = app.now + 100;
        (void)pk_app_button_edge(&app, t0, PK_KEY_DOT, PK_EDGE_PRESS);
        (void)pk_app_button_edge(&app, t0 + 40, PK_KEY_DOT, PK_EDGE_RELEASE);
        (void)pk_app_button_edge(&app, t0 + 120, PK_KEY_DOT, PK_EDGE_PRESS);
        (void)pk_app_button_edge(&app, t0 + 160, PK_KEY_DOT, PK_EDGE_RELEASE);
    }
    expect(app.page == PK_PAGE_STATUS,
           "connected double-click opens the status page (new gesture contract)");
}

static void test_real_vehicle_ux_and_park_fixes(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "add VIN_A");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;

    /* 1. Flash & Honk work without Infotainment park lease when not moving */
    expect(pk_app_guard(&app, PK_ACTION_FLASH, NULL) == NULL,
           "FLASH allowed without park lease when stationary");
    expect(pk_app_guard(&app, PK_ACTION_HONK, NULL) == NULL,
           "HONK allowed without park lease when stationary");

    /* 2. Omitted speed is not zero. A present 0 with P qualifies for 60s. */
    expect(pk_app_request_qualification(&app, false, false), "start qualification");
    {
        pk_qualify_sample_t s = {
            .request_id = app.pending.id,
            .scope = app.pending.scope,
            .shift = PK_SHIFT_P,
            .speed_present = false,
            .speed_float_present = false,
            .sample_present = true,
            .sample = app.now,
            .clock_verified = true,
            .adapter_verified = true,
        };
        expect(!pk_app_qualification_result(&app, &s),
               "proto3 omitted speed is not a stopped proof");
    }
    expect(!pk_app_parked(&app), "omitted speed leaves park unproven");
    expect(pk_app_request_qualification(&app, false, false), "retry qualification");
    {
        pk_qualify_sample_t s = {
            .request_id = app.pending.id,
            .scope = app.pending.scope,
            .shift = PK_SHIFT_P,
            .speed_present = true,
            .speed = 0,
            .sample_present = true,
            .sample = app.now,
            .clock_verified = true,
            .adapter_verified = true,
        };
        expect(pk_app_qualification_result(&app, &s),
               "present zero speed with P qualifies as parked");
    }
    expect(pk_app_parked(&app), "parked lease active immediately");
    pk_app_tick(&app, app.now + 10000);
    expect(pk_app_parked(&app), "parked lease survives 10s (< 60s)");

    /* 3. Clean LOCK with omitted closureStatuses does NOT trigger red alert & auto-returns to MAIN */
    app.page = PK_PAGE_MAIN;
    app.vcsec_status_valid = false;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "commit LOCK");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "LOCK accepted -> start verify");
    {
        pk_closure_t cls[PK_CLOSURE_COUNT] = {0};
        pk_lock_packet_t pkt = {
            .request_id = app.pending.id,
            .authenticated = true,
            .correlated = true,
            .adapter_verified = true,
            .lock = PK_LOCK_READ_LOCKED,
            .closures = cls,
            .closures_complete = false,
            .complete = true,
        };
        expect(pk_app_lock_packet(&app, &pkt), "lock verify packet accepted");
    }
    expect(!app.lock_alert_active && app.page == PK_PAGE_MAIN &&
               !app.result_auto_return && app.message == PK_MSG_LOCK_CONFIRMED,
           "confirmed clean LOCK stays inline on MAIN with no alert");
    pk_app_primary(&app);
    expect(pk_app_primary(&app) == PK_ACTION_UNLOCK,
           "clean LOCK leaves UNLOCK as primary action");
}

static void test_park_requires_explicit_p(void)
{
    static const pk_shift_t bad_shifts[] = { PK_SHIFT_ABSENT, PK_SHIFT_INVALID };
    size_t i;

    for (i = 0; i < sizeof(bad_shifts) / sizeof(bad_shifts[0]); i++) {
        pk_app_t app;
        pk_ble_adapter_t ad;
        pk_qualify_sample_t s;

        pk_app_init(&app, PK_VEHICLE_NONE, false);
        attach_radio(&app, &ad);
        expect(add_and_select(&app, VIN_A) >= 0, "park gate: add VIN_A");
        pk_app_set_connected(&app, true);
        pk_app_tick(&app, 1000);
        app.awake = true;

        expect(pk_app_request_qualification(&app, false, false),
               "park gate: start qualification");
        memset(&s, 0, sizeof(s));
        s.request_id = app.pending.id;
        s.scope = app.pending.scope;
        s.shift = bad_shifts[i];
        s.speed_present = true;
        s.speed = 0;
        s.sample_present = true;
        s.sample = app.now;
        s.clock_verified = true;
        s.adapter_verified = true;

        expect(!pk_app_qualification_result(&app, &s),
               "shift absent/invalid must not qualify as parked");
        expect(!pk_app_parked(&app), "park gate stays closed for unknown shift");
    }
}

static size_t build_session_info(uint8_t *out, size_t cap,
                                 const uint8_t vehicle_pub[PK_PROTO_PUBKEY_LEN],
                                 uint32_t counter,
                                 const uint8_t epoch[PK_PROTO_EPOCH_LEN],
                                 uint32_t clock_time)
{
    size_t pos = 0;

    if (!out || !vehicle_pub || !epoch ||
        cap < 2u + 2u + PK_PROTO_PUBKEY_LEN + 2u + PK_PROTO_EPOCH_LEN + 5u) {
        return 0;
    }
    out[pos++] = 0x08; /* counter (test values stay < 128) */
    out[pos++] = (uint8_t)counter;
    out[pos++] = 0x12; /* public key */
    out[pos++] = PK_PROTO_PUBKEY_LEN;
    memcpy(out + pos, vehicle_pub, PK_PROTO_PUBKEY_LEN);
    pos += PK_PROTO_PUBKEY_LEN;
    out[pos++] = 0x1A; /* epoch */
    out[pos++] = PK_PROTO_EPOCH_LEN;
    memcpy(out + pos, epoch, PK_PROTO_EPOCH_LEN);
    pos += PK_PROTO_EPOCH_LEN;
    out[pos++] = 0x25; /* clock_time fixed32 */
    out[pos++] = (uint8_t)(clock_time & 0xFFu);
    out[pos++] = (uint8_t)((clock_time >> 8) & 0xFFu);
    out[pos++] = (uint8_t)((clock_time >> 16) & 0xFFu);
    out[pos++] = (uint8_t)((clock_time >> 24) & 0xFFu);
    return pos;
}

static bool apply_session_info(pk_session_ctx_t *ctx, pk_proto_session_t *sess,
                               const uint8_t priv_a[PK_PROTO_PRIVKEY_LEN],
                               const uint8_t vehicle_pub[PK_PROTO_PUBKEY_LEN],
                               const uint8_t session_key[PK_PROTO_SESSION_KEY_LEN],
                               uint32_t counter, uint32_t clock_time,
                               const uint8_t epoch[PK_PROTO_EPOCH_LEN],
                               const uint8_t request_uuid[PK_PROTO_UUID_LEN],
                               bool has_request_uuid,
                               const uint8_t challenge[PK_PROTO_UUID_LEN],
                               int slot, pk_ms_t now)
{
    uint8_t info[128];
    uint8_t tag[32];
    size_t info_len;
    pk_proto_routable_t rmsg;
    pk_session_apply_t in;
    pk_proto_session_info_msg_t msg;

    info_len = build_session_info(info, sizeof(info), vehicle_pub, counter, epoch, clock_time);
    if (info_len == 0) {
        return false;
    }
    msg = (pk_proto_session_info_msg_t){
        .vin              = VIN_A,
        .challenge        = challenge,
        .challenge_len    = PK_PROTO_UUID_LEN,
        .encoded_info     = info,
        .encoded_info_len = info_len,
    };
    if (!pk_proto_session_info_tag_compute(session_key, &msg, tag)) {
        return false;
    }
    memset(&rmsg, 0, sizeof(rmsg));
    if (info_len > sizeof(rmsg.session_info_bytes)) {
        return false;
    }
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    rmsg.has_session_info = true;
    memcpy(rmsg.session_info_bytes, info, info_len);
    rmsg.session_info_len = info_len;
    rmsg.has_session_info_tag = true;
    memcpy(rmsg.session_info_tag, tag, sizeof(tag));
    rmsg.session_info_tag_len = sizeof(tag);
    if (has_request_uuid) {
        rmsg.has_request_uuid = true;
        memcpy(rmsg.request_uuid, request_uuid, PK_PROTO_UUID_LEN);
    }
    in = (pk_session_apply_t){
        .slot    = slot,
        .domain  = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin     = VIN_A,
        .privkey = priv_a,
        .rmsg    = &rmsg,
        .now_ms  = now,
        .session_now_ms = now,
    };
    return pk_session_apply(ctx, &in, sess);
}

static void test_session_core_attribution_and_resync(void)
{
    uint8_t priv_a[32], pub_a[65];
    uint8_t priv_b[32], pub_b[65];
    uint8_t priv_b2[32], pub_b2[65];
    uint8_t key_ab[16], key_ab2[16];
    uint8_t seed[32];
    uint8_t epoch[16], epoch2[16];
    uint8_t hs_uuid[16], req_uuid[16], wrong_uuid[16];
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    int i;

    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(i + 1);
    }
    expect(pk_proto_p256_keygen(priv_a, pub_a, seed), "session: client keygen");
    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0xA0 + i);
    }
    expect(pk_proto_p256_keygen(priv_b, pub_b, seed), "session: vehicle keygen");
    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0x40 + i);
    }
    expect(pk_proto_p256_keygen(priv_b2, pub_b2, seed), "session: second vehicle keygen");
    expect(pk_proto_ecdh_derive_session_key(priv_a, pub_b, key_ab),
           "session: ECDH client/vehicle");
    expect(pk_proto_ecdh_derive_session_key(priv_a, pub_b2, key_ab2),
           "session: ECDH client/second vehicle");
    for (i = 0; i < 16; i++) {
        epoch[i] = (uint8_t)(0x10 + i);
        epoch2[i] = (uint8_t)(0x80 + i);
        hs_uuid[i] = (uint8_t)(0x30 + i);
        req_uuid[i] = (uint8_t)(0x50 + i);
        wrong_uuid[i] = (uint8_t)(0x70 + i);
    }

    /* 1. A recent handshake UUID can authenticate an unattributed SessionInfo. */
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, false, hs_uuid, 1, 1100),
           "session: handshake UUID applies SessionInfo");
    expect(sess.valid && sess.counter == 10 && sess.clock_time == 1000,
           "session: state populated on apply");
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, req_uuid, false, hs_uuid, 1, 1200),
           "session: handshake is consumed after apply");

    /* 2. Wrong UUID, stale challenge and wrong slot are rejected. */
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, wrong_uuid, true, wrong_uuid, 1, 1100),
           "session: unknown request UUID rejected");
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, hs_uuid, false, hs_uuid, 1, 1100),
           "session: rejected attribution keeps the handshake");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, hs_uuid, false, hs_uuid, 1, 5000),
           "session: stale handshake rejected");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, hs_uuid, false, hs_uuid, 2, 1100),
           "session: handshake bound to slot");

    /* 3. Signed request UUID may authenticate; unsigned may only correlate. */
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, true, req_uuid, 1, 1100),
           "session: signed request UUID applies SessionInfo");
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, req_uuid, true, req_uuid, 1, 1150),
           "session: signed challenge is one-shot");
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, wrong_uuid, false, 1100);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, true, req_uuid, 1, 1200),
           "session: unsigned probe does not erase signed challenge");
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, false, 1000);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                               epoch, req_uuid, true, req_uuid, 1, 1100),
           "session: unsigned request UUID cannot authenticate SessionInfo");

    /* 4. Reply correlation uses the last request for this slot/domain. */
    pk_session_init(&ctx);
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, req_uuid, PK_PROTO_UUID_LEN);
    expect(pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: matching request UUID correlates");
    rmsg.has_request_uuid = false;
    expect(pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: absent request UUID correlates to live request");
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, wrong_uuid, PK_PROTO_UUID_LEN);
    expect(!pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: wrong request UUID rejected");
    memcpy(rmsg.request_uuid, req_uuid, PK_PROTO_UUID_LEN);
    expect(!pk_session_correlate(&ctx, 2, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: correlation bound to slot");
    rmsg.from_domain = PK_PROTO_DOMAIN_INFOTAINMENT;
    expect(!pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: correlation bound to domain");
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    memcpy(rmsg.request_uuid, req_uuid, PK_PROTO_UUID_LEN);
    rmsg.has_request_uuid = true;
    expect(pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: correlation restored after domain check");

    /* 5. Resync faults drop session and attribution; a fresh SessionInfo wins. */
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, false, hs_uuid, 1, 1100),
           "session: prepare fault session");
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1100);
    expect(pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                     PK_PROTO_FAULT_INCORRECT_EPOCH, false),
           "session: resync fault clears session");
    expect(!sess.valid, "session: cleared session invalid");
    expect(!pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: resync fault clears attribution");

    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, false, hs_uuid, 1, 1100),
           "session: prepare fresh-session fault");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 2000);
    expect(!pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                      PK_PROTO_FAULT_INCORRECT_EPOCH, true),
           "session: fresh SessionInfo keeps session");
    expect(sess.valid, "session: fresh session remains valid");
    expect(!pk_session_handshake_recent(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        2000, 4000),
           "session: fault clears handshake even with fresh session");

    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 2000);
    expect(!pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                      PK_PROTO_FAULT_BUSY, false),
           "session: busy is not a resync fault");
    expect(sess.valid, "session: busy keeps session");
    expect(pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                     PK_PROTO_FAULT_UNKNOWN_KEY, false),
           "session: key fault clears session");
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    sess.valid = true;
    expect(pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                     PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES, false),
           "session: privilege fault clears session");
    sess.valid = true;
    expect(pk_session_clear_on_fault(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &sess,
                                     PK_PROTO_FAULT_REQUIRES_RESPONSE_ENCRYPTION, false),
           "session: response-encryption fault clears session");

    /* 6. Epoch/counter monotonicity and public-key replacement. */
    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 1000,
                              epoch, req_uuid, false, hs_uuid, 1, 1100),
           "session: counter baseline");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1200);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 9, 1100,
                              epoch, req_uuid, false, hs_uuid, 1, 1300),
           "session: lower counter accepted without regression");
    expect(sess.counter == 10, "session: counter never regresses");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1400);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 1, 1200,
                              epoch2, req_uuid, false, hs_uuid, 1, 1500),
           "session: new epoch accepted");
    expect(sess.counter == 1, "session: new epoch resets counter");
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1600);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b2, key_ab2, 2, 1300,
                               epoch2, req_uuid, false, hs_uuid, 1, 1700),
           "session: vehicle public-key replacement rejected");
    expect(sess.valid && memcmp(sess.vehicle_pubkey, pub_b, PK_PROTO_PUBKEY_LEN) == 0,
           "session: rejected replacement keeps old key and validity");

    memset(&sess, 0, sizeof(sess));
    memcpy(sess.epoch, epoch, PK_PROTO_EPOCH_LEN);
    memcpy(sess.vehicle_pubkey, pub_b, PK_PROTO_PUBKEY_LEN);
    sess.valid = false;
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1800);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b2, key_ab2, 2, 1300,
                               epoch2, req_uuid, false, hs_uuid, 1, 1900),
           "session: rejected replacement on empty validity");
    expect(!sess.valid, "session: rejection does not set valid");

    /* 7. A dropped link forgets attribution but keeps the stored session. */
    pk_session_init(&ctx);
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    pk_session_on_disconnect(&ctx, 1);
    expect(!pk_session_handshake_recent(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        1100, 4000),
           "session: disconnect clears handshake");
    expect(!pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: disconnect clears request attribution");

    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    pk_session_on_disconnect(&ctx, PK_VEHICLE_NONE);
    expect(!pk_session_handshake_recent(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        1100, 4000),
           "session: disconnect-all clears handshake");
    expect(!pk_session_correlate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg),
           "session: disconnect-all clears attribution");
}

static void test_session_fault_gate(void)
{
    uint8_t uuid[16], route[16], wrong_route[16];
    pk_session_ctx_t ctx;
    pk_proto_routable_t rmsg;
    int i;

    for (i = 0; i < 16; i++) {
        uuid[i] = (uint8_t)(0x20 + i);
        route[i] = (uint8_t)(0x40 + i);
        wrong_route[i] = (uint8_t)(0x60 + i);
    }
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, uuid, sizeof(uuid));
    rmsg.has_to_routing_addr = true;
    memcpy(rmsg.to_routing_addr, route, sizeof(route));

    pk_session_init(&ctx);
    expect(pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                 1000, 1000, true, 1, 2000, uuid, route,
                                 PK_ACTION_STATUS_READ,
                                 PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: live matching txn accepted");
    expect(!pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                  3000, 3000, true, 1, 2000, uuid, route,
                                  PK_ACTION_STATUS_READ,
                                  PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: expired txn rejected");
    expect(!pk_session_fault_gate(&ctx, 2, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                  1000, 1000, true, 1, 2000, uuid, route,
                                  PK_ACTION_STATUS_READ,
                                  PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: wrong txn slot rejected");
    memcpy(rmsg.request_uuid, wrong_route, sizeof(uuid));
    expect(!pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                  1000, 1000, true, 1, 2000, uuid, route,
                                  PK_ACTION_STATUS_READ,
                                  PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: wrong txn UUID rejected");
    memcpy(rmsg.request_uuid, uuid, sizeof(uuid));
    expect(pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                 1000, 1000, true, 1, 2000, uuid, route,
                                 PK_ACTION_STATUS_READ,
                                 PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, true),
           "gate: fresh session accepted");

    /* No-UUID status fault25 needs the txn routing address. */
    rmsg.has_request_uuid = false;
    expect(pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                 1000, 1000, true, 1, 2000, uuid, route,
                                 PK_ACTION_STATUS_READ,
                                 PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: matching routing address attributes uuidless fault25");
    expect(!pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                  1000, 1000, true, 1, 2000, uuid, wrong_route,
                                  PK_ACTION_STATUS_READ,
                                  PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: wrong routing address rejected");
    rmsg.has_to_routing_addr = false;
    expect(!pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                  1000, 1000, true, 1, 2000, uuid, route,
                                  PK_ACTION_STATUS_READ,
                                  PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: absent routing address cannot attribute uuidless fault25");

    /* A live handshake can attribute a no-UUID fault without an active txn. */
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, uuid, 1000);
    expect(pk_session_fault_gate(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, &rmsg,
                                 1000, 1100, false, 1, 0, uuid, route,
                                 PK_ACTION_STATUS_READ,
                                 PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false),
           "gate: live handshake attributes no-UUID fault");
}

static void test_session_sync_timestamp(void)
{
    uint8_t priv_a[32], pub_a[65], priv_b[32], pub_b[65];
    uint8_t key_ab[16], seed[32], epoch[16], uuid[16];
    uint8_t info[128], tag[32];
    size_t info_len;
    pk_proto_session_info_msg_t msg;
    pk_proto_routable_t rmsg;
    pk_session_apply_t in;
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    int i;

    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(i + 3);
    }
    expect(pk_proto_p256_keygen(priv_a, pub_a, seed), "sync: client keygen");
    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0x90 + i);
    }
    expect(pk_proto_p256_keygen(priv_b, pub_b, seed), "sync: vehicle keygen");
    expect(pk_proto_ecdh_derive_session_key(priv_a, pub_b, key_ab), "sync: ECDH");
    for (i = 0; i < 16; i++) {
        epoch[i] = (uint8_t)(0x20 + i);
        uuid[i] = (uint8_t)(0x60 + i);
    }
    info_len = build_session_info(info, sizeof(info), pub_b, 10, epoch, 1000);
    expect(info_len > 0, "sync: build SessionInfo");
    msg = (pk_proto_session_info_msg_t){
        .vin = VIN_A,
        .challenge = uuid,
        .challenge_len = PK_PROTO_UUID_LEN,
        .encoded_info = info,
        .encoded_info_len = info_len,
    };
    expect(pk_proto_session_info_tag_compute(key_ab, &msg, tag), "sync: compute tag");
    memset(&rmsg, 0, sizeof(rmsg));
    expect(info_len <= sizeof(rmsg.session_info_bytes), "sync: info_len fits");
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    rmsg.has_session_info = true;
    memcpy(rmsg.session_info_bytes, info, info_len);
    rmsg.session_info_len = info_len;
    rmsg.has_session_info_tag = true;
    memcpy(rmsg.session_info_tag, tag, sizeof(tag));
    rmsg.session_info_tag_len = sizeof(tag);

    pk_session_init(&ctx);
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, uuid, 1000);
    memset(&sess, 0, sizeof(sess));
    in = (pk_session_apply_t){
        .slot = 1,
        .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VIN_A,
        .privkey = priv_a,
        .rmsg = &rmsg,
        .now_ms = 1100,
        .session_now_ms = 900,
    };
    expect(pk_session_apply(&ctx, &in, &sess), "sync: apply SessionInfo");
    expect(sess.sync_local_ms == 900 && sess.clock_time == 1000,
           "sync: app watermark stored separately from challenge time");
}

static void test_session_fault_attribution(void)
{
    uint8_t hs_uuid[16], req_uuid[16], wrong_uuid[16];
    pk_session_ctx_t ctx;
    pk_proto_routable_t rmsg;
    int i;

    for (i = 0; i < 16; i++) {
        hs_uuid[i] = (uint8_t)(0x10 + i);
        req_uuid[i] = (uint8_t)(0x40 + i);
        wrong_uuid[i] = (uint8_t)(0x80 + i);
    }
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, hs_uuid, PK_PROTO_UUID_LEN);

    pk_session_init(&ctx);
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                    &rmsg, 1100),
           "fault: handshake UUID correlates");
    memcpy(rmsg.request_uuid, wrong_uuid, PK_PROTO_UUID_LEN);
    expect(!pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 1100),
           "fault: wrong UUID rejected");
    memcpy(rmsg.request_uuid, hs_uuid, PK_PROTO_UUID_LEN);
    expect(!pk_session_fault_matches(&ctx, 2, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 1100),
           "fault: wrong slot rejected");
    rmsg.from_domain = PK_PROTO_DOMAIN_INFOTAINMENT;
    expect(!pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 1100),
           "fault: wrong domain rejected");
    rmsg.from_domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY;
    expect(!pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 5001),
           "fault: stale handshake rejected");

    rmsg.has_request_uuid = false;
    expect(pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                    &rmsg, 1100),
           "fault: no UUID can use a live handshake");

    pk_session_init(&ctx);
    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_uuid, true, 1000);
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, req_uuid, PK_PROTO_UUID_LEN);
    expect(pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                    &rmsg, 1100),
           "fault: signed request UUID correlates");
    rmsg.has_request_uuid = false;
    expect(!pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 1100),
           "fault: no UUID cannot use a command request");

    pk_session_note_request(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, wrong_uuid, false, 1200);
    rmsg.has_request_uuid = true;
    memcpy(rmsg.request_uuid, req_uuid, PK_PROTO_UUID_LEN);
    expect(!pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     &rmsg, 1300),
           "fault: old signed fault is stale after an unsigned probe");
    memcpy(rmsg.request_uuid, wrong_uuid, PK_PROTO_UUID_LEN);
    expect(pk_session_fault_matches(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                    &rmsg, 1300),
           "fault: current unsigned request correlates");
}

static void test_session_and_fault_contract(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_proto_session_t sess;

    expect(pk_proto_fault_needs_resync(PK_PROTO_FAULT_INVALID_SIGNATURE) &&
           pk_proto_fault_needs_resync(PK_PROTO_FAULT_INVALID_COUNTER) &&
           pk_proto_fault_needs_resync(PK_PROTO_FAULT_INCORRECT_EPOCH) &&
           pk_proto_fault_needs_resync(PK_PROTO_FAULT_TIME_EXPIRED) &&
           pk_proto_fault_needs_resync(PK_PROTO_FAULT_TTL_TOO_LONG) &&
           pk_proto_fault_needs_resync(PK_PROTO_FAULT_REPEATED_COUNTER),
           "resync-able faults classified");
    expect(pk_proto_fault_is_key_error(PK_PROTO_FAULT_UNKNOWN_KEY) &&
           pk_proto_fault_is_key_error(PK_PROTO_FAULT_INACTIVE_KEY) &&
           pk_proto_fault_is_key_error(PK_PROTO_FAULT_INVALID_KEY_HANDLE),
           "key faults classified");
    expect(!pk_proto_fault_needs_resync(PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES),
           "privilege fault is not a session resync");

    memset(&sess, 0, sizeof(sess));
    sess.valid = true;
    sess.counter = 9;
    pk_proto_session_clear(&sess);
    expect(!sess.valid && sess.counter == 0, "session clear drops validity/counter");

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "fault: add VIN_A");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.vcsec_status_valid = true;
    app.vcsec_lock_state = PK_LOCK_READ_LOCKED;
    app.page = PK_PAGE_MAIN;

    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "fault: commit LOCK");
    expect(app.has_pending && app.busy, "fault: LOCK pending");
    pk_app_protocol_fault(&app, PK_PROTO_FAULT_INCORRECT_EPOCH, true);
    expect(!app.has_pending && !app.busy && app.page == PK_PAGE_RESULT,
           "fault clears pending and lands on RESULT");
    expect(app.message == PK_MSG_AUTH_UNCONFIRMED, "resync fault surfaces auth-unconfirmed");
    expect(!app.vcsec_status_valid, "fault invalidates stale VCSEC snapshot");

    pk_app_protocol_fault(&app, PK_PROTO_FAULT_UNKNOWN_KEY, true);
    expect(app.message == PK_MSG_NEED_APPROVAL, "unknown key asks for re-authorization");
}


static void test_direct_toggle_inline_feedback(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_closure_t cls[PK_CLOSURE_COUNT];

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "toggle: add VIN_A");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_SETTINGS;
    memset(&app.grant, 0, sizeof(app.grant));
    app.has_grant = true;
    app.grant.scope = pk_app_scope(&app);
    app.grant.until = app.now + 60000;
    app.grant.received = app.now;
    app.grant.proof = true;
    app.grant.parked = true;
    app.grant.speed_present = true;

    app.caps.sentry_known = true;
    app.caps.sentry_armed = false;
    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY), "sentry toggles directly");
    expect(app.has_pending && app.pending.action == PK_ACTION_SET_SENTRY &&
           app.draft.bool_enabled == PK_CAP_YES,
           "sentry target derived from last state");
    expect(app.page == PK_PAGE_SETTINGS && !app.edit_open,
           "sentry bypasses editor/confirm");

    app.has_pending = false;
    app.busy = false;
    app.page = PK_PAGE_SETTINGS;
    memset(&app.pending, 0, sizeof(app.pending));
    pk_ble_release_in_flight(&app);
    memset(&app.grant, 0, sizeof(app.grant));
    app.has_grant = true;
    app.grant.scope = pk_app_scope(&app);
    app.grant.until = app.now + 60000;
    app.grant.received = app.now;
    app.grant.proof = true;
    app.grant.parked = true;
    app.grant.speed_present = true;

    app.caps.port_open = PK_CAP_YES;
    expect(pk_app_activate_setting(&app, PK_SETTING_PORT), "port toggles directly");
    expect(app.has_pending && app.pending.action == PK_ACTION_CHARGE_PORT_CLOSE,
           "port target derived from open state");
    expect(app.page == PK_PAGE_SETTINGS && !app.edit_open,
           "port bypasses editor/confirm");

    app.has_pending = false;
    app.busy = false;
    app.page = PK_PAGE_MAIN;
    memset(&app.pending, 0, sizeof(app.pending));
    pk_ble_release_in_flight(&app);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "commit LOCK");
    expect(app.page == PK_PAGE_MAIN, "LOCK keeps main page (no WAIT)");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "LOCK accepted starts verify");
    expect(app.page == PK_PAGE_MAIN && app.pending.kind == PK_PENDING_LOCK_VERIFY,
           "verify stays inline");
    memset(cls, 0, sizeof(cls));
    {
        pk_lock_packet_t pkt = {
            .request_id = app.pending.id,
            .authenticated = true,
            .correlated = true,
            .adapter_verified = true,
            .lock = PK_LOCK_READ_LOCKED,
            .closures = cls,
            .closures_complete = true,
            .complete = true,
        };
        expect(pk_app_lock_packet(&app, &pkt), "clean LOCK verified");
    }
    expect(app.page == PK_PAGE_MAIN && !app.lock_alert_active &&
           app.message == PK_MSG_LOCK_CONFIRMED,
           "clean LOCK result stays inline");
}

static pk_ble_frame_t g_cap_frame;
static int g_cap_writes;

static bool fake_has_session(void *ctx, int slot, pk_domain_t domain)
{
    (void)ctx;
    (void)slot;
    (void)domain;
    return true;
}

static uint16_t g_test_mtu = 247;

static bool fake_get_mtu(void *ctx, uint16_t *out)
{
    (void)ctx;
    if (!out) {
        return false;
    }
    *out = g_test_mtu;
    return true;
}

static pk_err_t capture_write(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    g_cap_frame = *frame;
    g_cap_writes++;
    return PK_OK;
}

static void test_check_before_act_and_lease_origin(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_qualify_sample_t s;
    uint8_t expected[64];
    size_t expected_len;

    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "add VIN_A for precheck");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;

    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now),
           "park-gated gesture with no grant starts a check instead of refusing");
    expect(app.has_deferred && app.pending.kind == PK_PENDING_QUALIFICATION,
           "gesture is frozen behind exactly one qualification read");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_GET_QUALIFICATION,
           "only the drive-state read is sent");

    expected_len = pk_proto_encode_status_section(2, expected, sizeof(expected));
    expect(expected_len == g_cap_frame.payload_len &&
           memcmp(expected, g_cap_frame.payload, expected_len) == 0,
           "qualification reads one DriveState section, not a combined batch");

    s = (pk_qualify_sample_t){
        .request_id = app.pending.id,
        .scope = app.pending.scope,
        .shift = PK_SHIFT_P,
        .speed_present = true,
        .speed = 0,
        .sample_present = true,
        .sample = (int64_t)app.now - 1000,
        .clock_verified = true,
        .adapter_verified = true,
    };
    expect(pk_app_qualification_result(&app, &s),
           "a recent section timestamp proves P");
    expect(app.grant.until == app.now + PK_PARK_LEASE_MS,
           "grant lease starts at receipt, not at the section timestamp");
    expect(g_cap_writes == 2 && g_cap_frame.action == PK_ACTION_FRUNK_OPEN,
           "the frozen gesture is replayed after P is proven");
}


static void test_check_before_act_from_side_and_settings(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_qualify_sample_t s;
    pk_ms_t t;

    /* Side-key frunk: the gesture must start the check, not be refused by a
     * pre-commit guard. */
    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "side precheck: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_MAIN;

    (void)pk_side_press(&app, PK_KEY_UP, app.now);
    t = app.now + PK_DANGER_HOLD_MS + 1;
    pk_app_tick(&app, t);
    pk_side_tick(&app, t);
    expect(app.has_deferred && app.pending.kind == PK_PENDING_QUALIFICATION,
           "side-key park action starts a qualification read");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_GET_QUALIFICATION,
           "side-key park action sends only the qualification read");

    s = (pk_qualify_sample_t){
        .request_id = app.pending.id,
        .scope = app.pending.scope,
        .shift = PK_SHIFT_P,
        .speed_present = true,
        .speed = 0,
        .sample_present = true,
        .sample = (int64_t)app.now,
        .clock_verified = true,
        .adapter_verified = true,
    };
    expect(pk_app_qualification_result(&app, &s), "side precheck: P accepted");
    expect(g_cap_writes == 2 && g_cap_frame.action == PK_ACTION_FRUNK_OPEN,
           "side-key gesture replays after P is proven");

    /* Settings window-vent: same single-gate rule. */
    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "settings precheck: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_SETTINGS;

    expect(pk_app_activate_setting(&app, PK_SETTING_VENT),
           "settings park action starts a check instead of refusing");
    expect(app.has_deferred && app.pending.kind == PK_PENDING_QUALIFICATION,
           "settings park action freezes behind one read");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_GET_QUALIFICATION,
           "settings park action sends only the qualification read");

    /* Direct sentry toggle uses the same commit gate. */
    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "sentry precheck: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_SETTINGS;
    app.caps.sentry_known = true;
    app.caps.sentry_armed = false;

    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY),
           "sentry toggle commits directly without a park precheck");
    expect(app.has_pending && app.pending.kind == PK_PENDING_COMMAND &&
           app.pending.action == PK_ACTION_SET_SENTRY,
           "sentry toggle sends without freezing behind a qualification read");
    expect(!app.has_deferred, "sentry toggle has no deferred gesture");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_SET_SENTRY,
           "sentry toggle sends exactly one command");
}


static void test_stale_park_grant_rechecks(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "stale grant: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 10000);
    app.awake = true;
    grant_park(&app);
    app.grant.received = app.now - (PK_PARK_FRESH_MS + 1u);

    expect(pk_app_parked(&app), "old proof still inside the display lease");
    expect(!pk_app_park_fresh(&app), "old proof is not fresh for a real action");

    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now),
           "stale park proof starts a fresh check");
    expect(app.has_deferred && app.pending.kind == PK_PENDING_QUALIFICATION,
           "action waits for the fresh drive read");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_GET_QUALIFICATION,
           "only a fresh qualification read is sent");
}


static void test_stale_vehicle_sample_rejected(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_qualify_sample_t s;

    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "stale sample: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 10000);
    app.awake = true;
    expect(pk_app_request_qualification(&app, false, false),
           "transaction proof: start qualification");
    s = (pk_qualify_sample_t){
        .request_id = app.pending.id,
        .scope = app.pending.scope,
        .shift = PK_SHIFT_P,
        .speed_present = true,
        .speed = 0,
        .sample_present = true,
        /* Wall-clock age is not part of the proof; the reply is bound to the
         * one-shot request above. A stale wall-clock stamp must not reject it. */
        .sample = (int64_t)app.now - 60000, /* wall-clock age is ignored */
        .clock_verified = true,
        .adapter_verified = true,
    };
    expect(pk_app_qualification_result(&app, &s),
           "a live correlated reply qualifies regardless of wall-clock age");
    expect(pk_app_parked(&app), "transaction-bound P proof opens the gate");
}

static void test_status_reads_one_section_per_request(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    uint8_t expected[64];
    size_t expected_len;
    int section;

    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "add VIN_A for status");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);

    pk_app_open_status(&app, PK_ORIGIN_LIST);
    for (section = 0; section < PK_STATUS_SECTION_COUNT; section++) {
        expected_len = pk_proto_encode_status_section(section, expected, sizeof(expected));
        expect(g_cap_writes == section + 1 &&
               g_cap_frame.action == PK_ACTION_STATUS_READ &&
               g_cap_frame.payload_len == expected_len &&
               memcmp(g_cap_frame.payload, expected, expected_len) == 0,
               "status request carries exactly one VehicleData section");
        if (section + 1 < PK_STATUS_SECTION_COUNT) {
            expect(pk_app_status_advance(&app), "status advances to the next section");
            pk_app_status_pump(&app);
        } else {
            expect(!pk_app_status_advance(&app), "last section completes the page");
        }
    }
    expect(!app.has_pending && strcmp(app.status_phase, "complete") == 0,
           "status page completes after the last section");
}

static void test_plan_contract(void)
{
    const pk_plan_t *plan;
    int section;
    int action;

    expect(pk_plan_deadline(PK_PLAN_STATUS_SECTION) == PK_STATUS_READ_MS,
           "plan: status deadline");
    expect(pk_plan_deadline(PK_PLAN_QUALIFY) == PK_QUALIFICATION_READ_MS,
           "plan: qualification deadline");
    expect(pk_plan_deadline(PK_PLAN_WAKE_THEN_READ) == PK_WAKE_THEN_READ_MS,
           "plan: wake deadline");
    expect(pk_plan_deadline(PK_PLAN_COMMAND) == PK_COMMAND_WAIT_MS,
           "plan: command deadline");
    expect(pk_plan_deadline(PK_PLAN_LOCK_VERIFY) == PK_LOCK_VERIFY_MS,
           "plan: lock verify deadline");
    expect(pk_plan_deadline(PK_PLAN_UNLOCK_VERIFY) == PK_LOCK_VERIFY_MS,
           "plan: unlock verify deadline");
    expect(pk_plan_deadline(PK_PLAN_PAIR_VERIFY) == PK_PAIR_VERIFY_MS,
           "plan: pair verify deadline");
    expect(pk_plan_get(PK_PLAN_COUNT) == NULL, "plan: invalid kind has no row");

    for (section = 0; section < PK_STATUS_SECTION_COUNT; section++) {
        plan = pk_plan_get(PK_PLAN_STATUS_SECTION);
        expect(plan != NULL, "plan: status row present");
        expect(pk_plan_status_section(section).max_request_bytes > 0 &&
               pk_plan_status_section(section).max_response_bytes > 0,
               "plan: status section has byte budgets");
        expect(pk_plan_status_section_fits(section, PK_PLAN_MTU_FALLBACK),
               "plan: status section fits the protocol MTU");
        expect(!pk_plan_status_section_fits(section, 23),
               "plan: status section cannot fit an unnegotiated MTU");
    }
    expect(pk_plan_status_section(-1).max_response_bytes == 0 &&
           pk_plan_status_section(PK_STATUS_SECTION_COUNT).max_response_bytes == 0,
           "plan: invalid section has no budget");
    expect(!pk_plan_fits_mtu(NULL, PK_PLAN_MTU_FALLBACK),
           "plan: null plan never fits");

    expect(pk_plan_for_action(PK_ACTION_LOCK)->kind == PK_PLAN_COMMAND,
           "plan: command action maps to command row");
    expect(pk_plan_for_action(PK_ACTION_STATUS_READ)->kind == PK_PLAN_STATUS_SECTION,
           "plan: status read maps to status row");
    expect(pk_plan_for_action(PK_ACTION_GET_QUALIFICATION)->kind == PK_PLAN_QUALIFY,
           "plan: qualification maps to qualify row");
    expect(pk_plan_for_action(PK_ACTION_VCSEC_WAKE_ONCE)->kind == PK_PLAN_WAKE_THEN_READ,
           "plan: wake maps to wake row");
    expect(pk_plan_for_action(PK_ACTION_PAIR_VERIFY_VCSEC)->kind == PK_PLAN_PAIR_VERIFY,
           "plan: pair verify maps to pair row");
    expect(pk_plan_request_fits(PK_ACTION_LOCK, 128) &&
           !pk_plan_request_fits(PK_ACTION_LOCK, 129),
           "plan: command request cap enforced");
    expect(pk_plan_request_fits(PK_ACTION_VCSEC_WAKE_ONCE, 32) &&
           !pk_plan_request_fits(PK_ACTION_VCSEC_WAKE_ONCE, 33),
           "plan: wake request cap enforced");

    expect(!pk_plan_replay_allowed(PK_ACTION_LOCK), "plan: lock is not auto-replayed");
    expect(!pk_plan_replay_allowed(PK_ACTION_UNLOCK),
           "plan: risky unlock never replays");
    expect(!pk_plan_replay_allowed(PK_ACTION_FRUNK_OPEN),
           "plan: dangerous frunk never replays");
    for (action = PK_ACTION_NONE; action <= PK_ACTION_PAIR_VERIFY_INFOTAINMENT; action++) {
        expect(!pk_plan_replay_allowed((pk_action_t)action),
               "plan: no action is product-replayed");
    }
}

static void setup_status_planner_test(pk_app_t *app, pk_ble_adapter_t *ad, bool with_mtu)
{
    g_cap_writes = 0;
    pk_app_init(app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(ad);
    ad->radio.write_once = capture_write;
    ad->radio.connect_once = fake_connect;
    ad->radio.disconnect = fake_disconnect;
    ad->radio.cancel = fake_cancel;
    ad->radio.has_session = fake_has_session;
    if (with_mtu) {
        ad->radio.get_mtu = fake_get_mtu;
    }
    pk_ble_adapter_attach(app, ad);
    expect(add_and_select(app, VIN_A) >= 0, "status planner: add VIN_A");
    ad->connected_slot = app->vehicle;
    pk_app_set_connected(app, true);
    pk_app_tick(app, 1000);
}

static void test_status_fault25_skips_section(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    uint8_t expected[64];
    uint8_t first_step;
    size_t expected_len;

    g_test_mtu = 247;
    setup_status_planner_test(&app, &ad, false);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_cap_writes == 1 && app.status_step == 0 &&
           app.has_pending && app.pending.kind == PK_PENDING_STATUS,
           "fault25: first status section sent");

    first_step = app.status_step;
    pk_app_protocol_fault(&app, PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false);
    expect(app.has_pending && app.pending.kind == PK_PENDING_STATUS,
           "fault25: page remains pending");
    expect(app.status_skip_mask & (uint8_t)(1u << first_step),
           "fault25: failed section marked skipped");
    expect(app.status_step > first_step && app.status_need_send,
           "fault25: next section queued instead of retrying");
    expect(app.page == PK_PAGE_STATUS, "fault25: does not jump to result page");

    expected_len = pk_proto_encode_status_section((int)app.status_step,
                                                  expected, sizeof(expected));
    pk_app_status_pump(&app);
    expect(g_cap_writes == 2 && g_cap_frame.action == PK_ACTION_STATUS_READ &&
           g_cap_frame.payload_len == expected_len &&
           memcmp(g_cap_frame.payload, expected, expected_len) == 0,
           "fault25: next request carries a different section");

    while (app.has_pending) {
        uint8_t step = app.status_step;
        pk_app_protocol_fault(&app, PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false);
        if (app.has_pending) {
            expect(app.status_step > step, "fault25: never retries the same section");
            pk_app_status_pump(&app);
        }
    }
    expect(app.status_skip_mask == (uint8_t)((1u << PK_STATUS_SECTION_COUNT) - 1u),
           "fault25: every rejected section recorded");
    expect(strcmp(app.status_phase, "too_large") == 0 && !app.has_pending,
           "fault25: page ends as too_large");
}

static void test_status_timeout_no_retry(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    int writes;

    g_test_mtu = 247;
    setup_status_planner_test(&app, &ad, false);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.has_pending && app.pending.kind == PK_PENDING_STATUS,
           "timeout: status pending");
    writes = g_cap_writes;
    pk_app_tick(&app, app.pending.deadline + 1);
    expect(!app.has_pending, "timeout: pending cleared");
    expect(strcmp(app.status_phase, "timeout") == 0,
           "timeout: status phase reports timeout");
    expect(app.message == PK_MSG_STATUS_TIMEOUT, "timeout: status message");
    expect(g_cap_writes == writes, "timeout: no retry write");
}

static void test_status_out_of_order_rejected(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_status_input_t in;
    pk_scope_t bad_scope;
    pk_id_t request_id;
    uint8_t step;

    g_test_mtu = 247;
    setup_status_planner_test(&app, &ad, false);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    request_id = app.pending.id;
    step = app.status_step;

    in = (pk_status_input_t){
        .request_id = request_id + 1u,
        .scope = &app.pending.scope,
        .name = "range",
        .value = 1,
        .present = true,
        .sample_present = true,
        .sampled = app.now,
        .unit_verified = true,
        .clock_verified = true,
    };
    expect(!pk_app_status_field(&app, &in), "order: wrong request id rejected");
    expect(app.field_count == 0 && app.status_step == step,
           "order: rejected reply does not mutate status");

    bad_scope = app.pending.scope;
    bad_scope.session++;
    in.request_id = request_id;
    in.scope = &bad_scope;
    expect(!pk_app_status_field(&app, &in), "order: wrong scope rejected");
    expect(app.field_count == 0, "order: wrong scope adds no field");

    in.scope = &app.pending.scope;
    expect(pk_app_status_field(&app, &in), "order: matching reply accepted");
    expect(app.field_count == 1, "order: matching reply stored");
}

static void test_status_mtu_paging(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    uint8_t expected[64];
    size_t expected_len;

    g_test_mtu = 130; /* payload 127: only closures/tires budgets fit */
    setup_status_planner_test(&app, &ad, true);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.has_pending && app.status_step == 3,
           "mtu: first fitting section is closures");
    expected_len = pk_proto_encode_status_section(3, expected, sizeof(expected));
    expect(g_cap_writes == 1 && g_cap_frame.payload_len == expected_len &&
           memcmp(g_cap_frame.payload, expected, expected_len) == 0,
           "mtu: request skips oversized sections");
    expect(pk_app_status_advance(&app), "mtu: advances to next fitting section");
    pk_app_status_pump(&app);
    expect(app.status_step == 4 && g_cap_writes == 2,
           "mtu: tires section follows");
    expect(!pk_app_status_advance(&app) &&
           strcmp(app.status_phase, "complete") == 0,
           "mtu: page completes when no section fits");

    g_test_mtu = 80; /* payload 77: no section budget fits */
    setup_status_planner_test(&app, &ad, true);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(!app.has_pending && g_cap_writes == 0 &&
           strcmp(app.status_phase, "too_large") == 0,
           "mtu: no fitting section rejects the read without writing");
    g_test_mtu = 247;
}

static void test_status_mtu_shrink_during_pump(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    g_test_mtu = 247;
    setup_status_planner_test(&app, &ad, true);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_cap_writes == 1 && app.status_step == 0,
           "mtu shrink: first section sent at full MTU");
    expect(pk_app_status_advance(&app) && app.status_need_send,
           "mtu shrink: next section queued");

    g_test_mtu = 80;
    pk_app_status_pump(&app);
    expect(!app.has_pending &&
           strcmp(app.status_phase, "too_large") == 0 &&
           app.message == PK_MSG_RESPONSE_TOO_LARGE,
           "mtu shrink: blocked write terminates as too_large");
    expect(g_cap_writes == 1, "mtu shrink: no write attempted for a non-fitting section");
    expect((app.status_too_large_mask & (uint8_t)(1u << 1)) == 0,
           "mtu shrink: temporary block is not learned as vehicle fault25");
    g_test_mtu = 247;
    pk_app_exit_status(&app);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.status_step == 0 && g_cap_writes == 2,
           "mtu shrink: re-open retries the section after MTU recovery");
}

static void test_status_learned_fault25(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    int writes_before;

    g_test_mtu = 247;
    setup_status_planner_test(&app, &ad, false);
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.status_step == 0, "learned fault25: starts at section 0");
    pk_app_protocol_fault(&app, PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED, false);
    expect(app.status_too_large_mask & 0x01u,
           "learned fault25: section 0 excluded for this connection");

    pk_app_exit_status(&app);
    writes_before = g_cap_writes;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.status_step == 1 && g_cap_writes == writes_before + 1,
           "learned fault25: re-open skips the excluded section");

    pk_app_set_connected(&app, false);
    expect(app.status_too_large_mask == 0,
           "learned fault25: reconnect clears the learned exclusions");
    g_test_mtu = 247;
}

static void test_plaintext_vehicledata_parses_to_p(void)
{
    /* RoutableMessage payload 2 (VehicleData) -> field 5 (DriveState) ->
     * oneof shift field 1, value 2 = P. This is the plaintext shape a vehicle
     * that ignores FLAG_ENCRYPT_RESPONSE returns; it must parse without AES. */
    static const uint8_t k_drive_p[] = { 0x12, 0x06, 0x2A, 0x04, 0x0A, 0x02, 0x12, 0x00 };
    static const uint8_t k_drive_absent[] = { 0x12, 0x02, 0x2A, 0x00 };
    pk_proto_carserver_resp_t r;

    expect(pk_proto_parse_carserver_response(k_drive_p, sizeof(k_drive_p), &r),
           "plaintext VehicleData parses");
    expect(r.has_vehicle_data && r.has_drive_state && r.shift == PK_SHIFT_P,
           "plaintext drive state carries an explicit P");
    expect(pk_proto_parse_carserver_response(k_drive_absent, sizeof(k_drive_absent), &r) &&
           r.has_drive_state && r.shift == PK_SHIFT_ABSENT,
           "absent shift stays ABSENT and cannot qualify");
}

static void test_core_key_stays_on_vcsec(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "core-key VIN");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;

    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "lock commit");
    expect(g_cap_writes == 1 && g_cap_frame.domain == PK_DOMAIN_VCSEC &&
           g_cap_frame.action == PK_ACTION_LOCK,
           "lock uses VCSEC only, never Infotainment");

    /* Fresh app for unlock: the lock above still owns the VCSEC slot. */
    g_cap_writes = 0;
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "core-key VIN 2");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    pk_app_vcsec_status(&app, false, PK_LOCK_READ_LOCKED);

    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, app.now), "unlock commit");
    expect(g_cap_writes == 1 && g_cap_frame.domain == PK_DOMAIN_VCSEC &&
           g_cap_frame.action == PK_ACTION_UNLOCK,
           "unlock uses VCSEC only, never Infotainment");
}

/* Expected contract row for every public action. The table is intentionally
 * explicit so a new action cannot silently inherit a default. */
static const struct {
    pk_action_t action;
    pk_risk_t risk;
    bool moving_blocked;
} k_expected_risk[] = {
    { PK_ACTION_NONE, PK_RISK_RISKY, false },
    { PK_ACTION_LOCK, PK_RISK_SAFE, false },
    { PK_ACTION_UNLOCK, PK_RISK_RISKY, true },
    { PK_ACTION_FRUNK_OPEN, PK_RISK_DANGEROUS, true },
    { PK_ACTION_TRUNK_MOVE, PK_RISK_DANGEROUS, true },
    { PK_ACTION_CLIMATE_ON, PK_RISK_RISKY, false },
    { PK_ACTION_PRECONDITION_MAX_ON, PK_RISK_RISKY, false },
    { PK_ACTION_CHARGE_PORT_OPEN, PK_RISK_DANGEROUS, true },
    { PK_ACTION_CHARGE_PORT_CLOSE, PK_RISK_DANGEROUS, true },
    { PK_ACTION_WINDOW_VENT, PK_RISK_DANGEROUS, true },
    { PK_ACTION_FLASH, PK_RISK_RISKY, true },
    { PK_ACTION_HONK, PK_RISK_RISKY, true },
    { PK_ACTION_SET_SENTRY, PK_RISK_RISKY, true },
    { PK_ACTION_SET_SEAT_HEAT, PK_RISK_RISKY, false },
    { PK_ACTION_SET_STEERING_HEAT, PK_RISK_RISKY, false },
    { PK_ACTION_GET_QUALIFICATION, PK_RISK_SAFE, false },
    { PK_ACTION_VCSEC_WAKE_ONCE, PK_RISK_SAFE, false },
    { PK_ACTION_VCSEC_GET_STATUS_ONCE, PK_RISK_SAFE, false },
    { PK_ACTION_STATUS_READ, PK_RISK_SAFE, false },
    { PK_ACTION_ADDKEY, PK_RISK_RISKY, false },
    { PK_ACTION_SELECT, PK_RISK_SAFE, false },
    { PK_ACTION_RETURN_MAIN, PK_RISK_SAFE, false },
    { PK_ACTION_RECONNECT, PK_RISK_SAFE, false },
    { PK_ACTION_NO_TARGET, PK_RISK_RISKY, false },
    { PK_ACTION_PAIR_VERIFY_VCSEC, PK_RISK_RISKY, false },
    { PK_ACTION_PAIR_VERIFY_INFOTAINMENT, PK_RISK_RISKY, false },
};

static void test_feedback_state_machine(void)
{
    pk_app_t app;

    expect(pk_feedback_state(NULL) == PK_FEEDBACK_IDLE,
           "feedback: null app is idle");
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.page = PK_PAGE_MAIN;
    app.message = PK_MSG_EMPTY;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_IDLE,
           "feedback: idle main is idle");

    app.page = PK_PAGE_WAIT;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_PROGRESS,
           "feedback: wait page is progress");

    app.page = PK_PAGE_RESULT;
    app.message = PK_MSG_CMD_REJECTED;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_FAILURE,
           "feedback: rejected is failure");

    app.message = PK_MSG_SENT_WAIT;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_PROGRESS,
           "feedback: sent-wait is progress");
    app.message = PK_MSG_LOCK_VERIFYING;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_PROGRESS,
           "feedback: lock verifying is progress");

    app.message = PK_MSG_CMD_ACCEPTED;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_UNVERIFIED,
           "feedback: accepted-but-unverified is not success");
    app.message = PK_MSG_LOCK_CONFIRMED;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_SUCCESS,
           "feedback: lock confirmed is success");
    app.message = PK_MSG_NEED_APPROVAL;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_FAILURE,
           "feedback: card approval request is failure");
    app.message = PK_MSG_RESPONSE_TOO_LARGE;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_FAILURE,
           "feedback: oversized response is failure");

    app.message = PK_MSG_EMPTY;
    app.has_pending = true;
    app.pending.kind = PK_PENDING_COMMAND;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_PROGRESS,
           "feedback: pending command is progress");
    app.has_pending = false;
    app.pending.kind = PK_PENDING_NONE;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_IDLE,
           "feedback: empty result is idle");

    app.page = PK_PAGE_WAIT;
    app.message = PK_MSG_CMD_REJECTED;
    expect(pk_feedback_state(&app) == PK_FEEDBACK_FAILURE,
           "feedback: failure outranks the wait surface");
}

static void test_feedback_classification(void)
{
    static const pk_msg_id_t k_failures[] = {
        PK_MSG_CMD_REJECTED, PK_MSG_CMD_TIMEOUT, PK_MSG_CMD_UNCONFIRMED,
        PK_MSG_LOCK_UNCONFIRMED, PK_MSG_LOCK_NO_LINK, PK_MSG_LOCK_UNLOCKED,
        PK_MSG_WAKE_FAIL, PK_MSG_WAKE_TIMEOUT, PK_MSG_AUTH_REJECTED,
        PK_MSG_AUTH_UNCONFIRMED, PK_MSG_NEED_APPROVAL, PK_MSG_RESPONSE_TOO_LARGE,
    };
    size_t i;

    for (i = 0; i < sizeof(k_failures) / sizeof(k_failures[0]); i++) {
        expect(pk_msg_is_failure(k_failures[i]), "feedback: failure classified");
    }
    expect(!pk_msg_is_failure(PK_MSG_EMPTY), "feedback: empty is not failure");
    expect(!pk_msg_is_failure(PK_MSG_SENT_WAIT), "feedback: pending is not failure");
    expect(!pk_msg_is_failure(PK_MSG_CMD_ACCEPTED), "feedback: accepted is not failure");
    expect(!pk_msg_is_failure(PK_MSG_LOCK_CONFIRMED), "feedback: confirmed is not failure");
}

static void test_risk_contract(void)
{
    const unsigned n = (unsigned)(sizeof(k_expected_risk) / sizeof(k_expected_risk[0]));
    unsigned i;
    bool every_action_covered = true;

    expect(n == (unsigned)PK_ACTION_PAIR_VERIFY_INFOTAINMENT + 1u,
           "risk table has one row per action");

    for (i = 0; i < n; i++) {
        pk_action_t a = k_expected_risk[i].action;
        pk_risk_t r = k_expected_risk[i].risk;
        unsigned class_hold = (r == PK_RISK_SAFE)
                                  ? 0u
                                  : (r == PK_RISK_RISKY ? PK_MAIN_HOLD_MS
                                                        : PK_DANGER_HOLD_MS);
        unsigned action_hold = (a == PK_ACTION_SELECT ||
                                a == PK_ACTION_RETURN_MAIN ||
                                a == PK_ACTION_RECONNECT)
                                   ? PK_MAIN_HOLD_MS
                                   : class_hold;

        expect(pk_action_risk(a) == r, "action risk class is pinned");
        expect(pk_risk_name(r)[0] != 'u', "risk class has a known name");
        expect(pk_action_is_park_gated(a) == (r == PK_RISK_DANGEROUS),
               "park gate derives from risk class");
        expect(!pk_risk_replay_allowed(pk_action_risk(a)),
               "no risk class is product-replayed");
        expect(pk_risk_hold_ms(r) == class_hold, "class hold is pinned");
        expect(pk_action_moving_blocked(a) == k_expected_risk[i].moving_blocked,
               "moving block is pinned");
        expect(pk_action_hold_ms(a) == action_hold, "gesture hold is pinned");
    }

    for (int a = PK_ACTION_NONE; a <= PK_ACTION_PAIR_VERIFY_INFOTAINMENT; a++) {
        unsigned matches = 0;
        for (i = 0; i < n; i++) {
            if ((int)k_expected_risk[i].action == a) {
                matches++;
            }
        }
        if (matches != 1) {
            every_action_covered = false;
        }
    }
    expect(every_action_covered, "every action has exactly one risk row");

    expect(!pk_risk_park_gated(PK_RISK_SAFE) && !pk_risk_park_gated(PK_RISK_RISKY) &&
           pk_risk_park_gated(PK_RISK_DANGEROUS),
           "only dangerous actions need park evidence");
    expect(!pk_risk_replay_allowed(PK_RISK_SAFE) &&
           !pk_risk_replay_allowed(PK_RISK_RISKY) &&
           !pk_risk_replay_allowed(PK_RISK_DANGEROUS),
           "SAFE is not a product replay right");

    /* Regression pins: trunk is a closure and must stay P-gated. */
    expect(pk_action_risk(PK_ACTION_TRUNK_MOVE) == PK_RISK_DANGEROUS,
           "trunk is dangerous");
    expect(pk_action_is_park_gated(PK_ACTION_TRUNK_MOVE),
           "trunk park gate is not lost");
    expect(pk_action_hold_ms(PK_ACTION_TRUNK_MOVE) == PK_DANGER_HOLD_MS,
           "trunk keeps the danger hold");
    expect(pk_action_hold_ms(PK_ACTION_LOCK) == 0, "safe command needs no hold");
    expect(pk_action_hold_ms(PK_ACTION_RETURN_MAIN) == PK_MAIN_HOLD_MS,
           "UI navigation keeps the base long-press");
}

/* A dropped link never re-sends LOCK, UNLOCK, or a closure. */
static void test_replay_is_lock_only(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "replay: add VIN_A");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.last_activity = app.now;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "replay: commit LOCK");
    pk_app_set_connected(&app, false);
    expect(!app.has_pending && app.message == PK_MSG_LOCK_UNCONFIRMED,
           "LOCK drop is unconfirmed and is not replayed");
    pk_app_set_connected(&app, true);
    expect(!app.has_pending, "reconnect does not restore the LOCK");

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "replay: add VIN_A 2");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.last_activity = app.now;
    pk_app_vcsec_status(&app, true, PK_LOCK_READ_LOCKED);
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, app.now), "replay: commit UNLOCK");
    pk_app_set_connected(&app, false);
    expect(!app.has_pending, "risky UNLOCK pending is dropped");
    pk_app_set_connected(&app, true);
    expect(!app.has_pending, "reconnect does not restore the UNLOCK");

    pk_app_init(&app, PK_VEHICLE_NONE, false);
    attach_radio(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "replay: add VIN_A 3");
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.last_activity = app.now;
    grant_park(&app);
    app.grant.received = app.now;
    intent = pk_app_make_intent(&app, PK_ACTION_TRUNK_MOVE);
    expect(pk_app_commit(&app, &intent, app.now), "replay: commit TRUNK");
    expect(app.has_pending && app.pending.action == PK_ACTION_TRUNK_MOVE,
           "replay: TRUNK owns the pending slot");
    pk_app_set_connected(&app, false);
    expect(!app.has_pending, "dangerous TRUNK pending is dropped");
    pk_app_set_connected(&app, true);
    expect(!app.has_pending, "reconnect does not restore the TRUNK");
}

/* Byte-level compliance against the canonical vectors in teslamotors/vehicle-command
 * pkg/protocol/protocol.md (Test keys, Handshake, Authorizing commands). These pin
 * ECDH->K, HMAC session-info key, handshake tag, metadata AAD and AES-GCM, which a
 * self-consistent test cannot catch (wrong tag values, endianness, signature type). */
static void test_official_protocol_vectors(void)
{
    uint8_t c_priv[PK_PROTO_PRIVKEY_LEN];
    uint8_t v_pub[PK_PROTO_PUBKEY_LEN];
    uint8_t k[PK_PROTO_SESSION_KEY_LEN];
    uint8_t sik[32], tag[32];
    uint8_t si[96], meta[64], aad[32], nonce[PK_PROTO_NONCE_LEN];
    uint8_t pt[8], ct[8], gtag[PK_PROTO_TAG_LEN];
    uint8_t challenge[PK_PROTO_UUID_LEN];
    uint8_t expect_buf[32];
    size_t n;
    pk_proto_session_info_msg_t msg;

    /* K = SHA1(Sx)[:16] from official client c and vehicle V. */
    expect(hex_decode("2538cdc29a97c19c1e99a637d6cf4f8c970c118b56ede1e6323e6d162c4b30db",
                      c_priv, sizeof(c_priv)) == sizeof(c_priv), "vec: decode client priv");
    expect(hex_decode("04c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d877d3a6aaa0e955166474af5d32c410f439a2234137ad1bb085fd4e8813c958f11d97",
                      v_pub, sizeof(v_pub)) == sizeof(v_pub), "vec: decode vehicle pub");
    expect(pk_proto_ecdh_derive_session_key(c_priv, v_pub, k), "vec: ECDH derive K");
    expect(hex_decode("1b2fce19967b79db696f909cff89ea9a", expect_buf, sizeof(expect_buf)) == 16 &&
           memcmp(k, expect_buf, 16) == 0,
           "vec: K == SHA1(Sx)[:16]");

    /* SESSION_INFO_KEY = HMAC-SHA256(K, "session info"). */
    pk_proto_hmac_sha256(k, 16, (const uint8_t *)"session info", 12, sik);
    expect(hex_decode("fceb679ee7bca756fcd441bf238bf2f338629b41d9eb9c67be1b32c9672ce300",
                      expect_buf, sizeof(expect_buf)) == 32 &&
           memcmp(sik, expect_buf, 32) == 0,
           "vec: SESSION_INFO_KEY");

    /* Handshake tag = HMAC-SHA256(SESSION_INFO_KEY, metadata || SessionInfo). */
    n = hex_decode("0806124104c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d877d3a6aaa0e955166474af5d32c410f439a2234137ad1bb085fd4e8813c958f11d971a104c463f9cc0d3d26906e982ed224adde6255a0a0000",
                   si, sizeof(si));
    expect(n == 92, "vec: decode SessionInfo");
    expect(hex_decode("1588d5a30eabc6f8fc9a951b11f6fd11", challenge, sizeof(challenge)) == sizeof(challenge),
           "vec: decode challenge");
    msg = (pk_proto_session_info_msg_t){
        .vin = "5YJ30123456789ABC",
        .challenge = challenge,
        .challenge_len = sizeof(challenge),
        .encoded_info = si,
        .encoded_info_len = n,
    };
    expect(pk_proto_session_info_tag_compute(k, &msg, tag), "vec: compute session info tag");
    expect(hex_decode("996c1fe38331be138f8039c194b14db2198846ed7d8251e6749284d7b32ea002",
                      expect_buf, sizeof(expect_buf)) == 32 &&
           memcmp(tag, expect_buf, 32) == 0,
           "vec: handshake tag");

    /* Command AES-GCM: AAD = SHA256(metadata); deterministic ciphertext/tag. */
    n = hex_decode("000105010103021135594a333031323334353637383941424303104c463f9cc0d3d26906e982ed224adde6040400000a5f050400000007070400000002ff",
                   meta, sizeof(meta));
    expect(n > 0, "vec: decode command metadata");
    pk_proto_sha256(meta, n, aad);
    expect(hex_decode("54785b6878a51a70acaf240199f92f482962a73a8c3170bfd2a94b1715d127b9",
                      expect_buf, sizeof(expect_buf)) == 32 &&
           memcmp(aad, expect_buf, 32) == 0,
           "vec: AAD == SHA256(metadata)");
    expect(hex_decode("dbf79447fa156674dae1caed", nonce, sizeof(nonce)) == sizeof(nonce), "vec: decode nonce");
    expect(hex_decode("120452020801", pt, sizeof(pt)) == 6, "vec: decode plaintext");
    expect(pk_proto_aes128_gcm_encrypt(k, nonce, aad, 32, pt, 6, ct, gtag), "vec: AES-GCM encrypt");
    expect(hex_decode("38038e8c0f2e", expect_buf, sizeof(expect_buf)) == 6 &&
           memcmp(ct, expect_buf, 6) == 0,
           "vec: AES-GCM ciphertext");
    expect(hex_decode("c228e0ff64991481db3a7bbc133696c5", expect_buf, sizeof(expect_buf)) == 16 &&
           memcmp(gtag, expect_buf, 16) == 0,
           "vec: AES-GCM tag");
}

static void test_session_info_clock_rollback_rejected(void)
{
    uint8_t priv_a[32], pub_a[65];
    uint8_t priv_b[32], pub_b[65];
    uint8_t key_ab[16];
    uint8_t epoch[16], hs_uuid[16];
    uint8_t seed[32];
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    int i;

    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(i + 1);
    }
    expect(pk_proto_p256_keygen(priv_a, pub_a, seed), "rollback: client keygen");
    for (i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0xA0 + i);
    }
    expect(pk_proto_p256_keygen(priv_b, pub_b, seed), "rollback: vehicle keygen");
    expect(pk_proto_ecdh_derive_session_key(priv_a, pub_b, key_ab), "rollback: ECDH");
    for (i = 0; i < 16; i++) {
        epoch[i] = (uint8_t)(0x10 + i);
        hs_uuid[i] = (uint8_t)(0x30 + i);
    }

    pk_session_init(&ctx);
    memset(&sess, 0, sizeof(sess));
    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1000);
    expect(apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 10, 2000,
                              epoch, hs_uuid, false, hs_uuid, 1, 1100),
           "rollback: baseline SessionInfo applies");
    expect(sess.clock_time == 2000u && sess.counter == 10u,
           "rollback: baseline clock and counter");

    pk_session_note_handshake(&ctx, 1, PK_PROTO_DOMAIN_VEHICLE_SECURITY, hs_uuid, 1200);
    expect(!apply_session_info(&ctx, &sess, priv_a, pub_b, key_ab, 11, 1500,
                               epoch, hs_uuid, false, hs_uuid, 1, 1300),
           "rollback: same-epoch clock regression discarded");
    expect(sess.clock_time == 2000u && sess.counter == 10u && sess.valid,
           "rollback: rejected SessionInfo leaves session untouched");
}

static void test_response_counter_reuse_rejected(void)
{
    /* D02 upgrade (replaces pre-S2 1-slot pk_proto_resp_counter_accept):
     * official vehicle-command 64-bit sliding anti-replay window rejects
     * A-B-A replays (0->1->0) and duplicates while supporting in-window
     * out-of-order packets up to age=64 (age=65 rejected). */
    pk_proto_rx_window_t w;

    pk_proto_rx_window_init(&w);
    expect(pk_proto_rx_window_accept(&w, 0u), "counter: 0 accepted");
    expect(!pk_proto_rx_window_accept(&w, 0u), "counter: duplicate 0 rejected");
    expect(pk_proto_rx_window_accept(&w, 1u), "counter: 1 accepted");
    expect(!pk_proto_rx_window_accept(&w, 0u), "counter: A-B-A replay 0->1->0 rejected");
    expect(pk_proto_rx_window_accept(&w, 100u), "counter: 100 accepted");
    expect(pk_proto_rx_window_accept(&w, 36u), "counter: in-window age=64 accepted");
    expect(!pk_proto_rx_window_accept(&w, 36u), "counter: duplicate 36 rejected");
    expect(!pk_proto_rx_window_accept(&w, 35u), "counter: out-of-window age=65 rejected");
    expect(!pk_proto_rx_window_accept(NULL, 9u), "counter: null rejected");
}

static void test_wake_then_and_toggle_ack(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "wake: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.page = PK_PAGE_SETTINGS;
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;

    g_cap_writes = 0;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE),
           "wake: infotainment gesture accepted");
    expect(app.has_pending && app.pending.kind == PK_PENDING_WAKE_THEN && app.has_deferred,
           "wake: infotainment command freezes behind one wake");
    expect(g_cap_writes == 1 && g_cap_frame.action == PK_ACTION_VCSEC_WAKE_ONCE,
           "wake: only a VCSEC wake is sent");

    app.pending.qphase = 1;
    app.pending.deadline = app.now - 1;
    pk_app_tick(&app, app.now);
    expect(g_cap_writes == 2 && g_cap_frame.action == PK_ACTION_CLIMATE_ON,
           "wake: frozen intent replays after settle");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "wake: replayed command completes");
    /* ACK is terminal for the app: no timer-based readback blocks the next
     * gesture. State is refreshed on the next explicit read. */
    expect(!app.busy && !app.has_pending && app.message == PK_MSG_CMD_ACCEPTED,
           "wake: ACK completes without a timer readback");

    /* VCSEC actions work asleep and never take the wake path. */
    g_cap_writes = 0;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "wake: lock commits");
    expect(app.pending.kind == PK_PENDING_COMMAND && g_cap_frame.action == PK_ACTION_LOCK,
           "wake: VCSEC command skips the wake");

    /* Toggle readback: ACK alone is not completion. */
    app.page = PK_PAGE_SETTINGS;
    app.message = PK_MSG_EMPTY;
    app.has_pending = true;
    app.busy = true;
    app.pending.kind = PK_PENDING_COMMAND;
    app.pending.action = PK_ACTION_SET_SENTRY;
    app.pending.bool_enabled = PK_CAP_YES;
    app.pending.id = app.serial;
    app.pending.scope = pk_app_scope(&app);
    app.pending.deadline = app.now + 5000;
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "sentry: ACK handled");
    expect(!app.has_pending && !app.busy && app.message == PK_MSG_CMD_ACCEPTED,
           "sentry: ACK is terminal, no timer readback");
}

static void test_vent_gesture_keeps_settings_page(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_qualify_sample_t s;

    /* Long-press the dangerous vent row: the page must stay in SETTINGS. */
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "vent gesture: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_SETTINGS;
    app.focus = PK_SETTING_VENT;

    g_cap_writes = 0;
    pk_app_button_edge(&app, 2000, PK_KEY_DOT, PK_EDGE_PRESS);
    pk_app_tick(&app, 2500); /* 500 ms long action */
    expect(app.page == PK_PAGE_SETTINGS, "vent gesture: page kept after long action");
    expect(app.has_deferred, "vent gesture: vent froze behind qualification");
    s = (pk_qualify_sample_t){
        .request_id = app.pending.id,
        .scope = app.pending.scope,
        .shift = PK_SHIFT_P,
        .speed_present = true,
        .speed = 0,
        .sample_present = true,
        .sample = app.now,
        .clock_verified = true,
        .adapter_verified = true,
    };
    expect(pk_app_qualification_result(&app, &s), "vent gesture: P proves");
    expect(app.page == PK_PAGE_SETTINGS, "vent gesture: page kept after replay");
    expect(g_cap_writes == 2 && g_cap_frame.action == PK_ACTION_WINDOW_VENT,
           "vent gesture: vent sent after proof");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "vent gesture: ack");
    expect(app.page == PK_PAGE_SETTINGS, "vent gesture: page kept after ack");
    pk_app_button_edge(&app, 2700, PK_KEY_DOT, PK_EDGE_RELEASE);
    pk_app_tick(&app, 3000);
    expect(app.page == PK_PAGE_SETTINGS, "vent gesture: page kept after release/deadline");
}

static void test_second_press_long_never_returns(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    /* Short tap (hold hint) then a second press inside 180 ms that becomes a
     * long press: second-press state must not turn the release into "back". */
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "second press: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_SETTINGS;
    app.focus = PK_SETTING_VENT;

    pk_app_button_edge(&app, 2000, PK_KEY_DOT, PK_EDGE_PRESS);
    pk_app_button_edge(&app, 2050, PK_KEY_DOT, PK_EDGE_RELEASE); /* short tap */
    pk_app_button_edge(&app, 2100, PK_KEY_DOT, PK_EDGE_PRESS);   /* within window */
    pk_app_tick(&app, 2650); /* long action fires on second press */
    expect(app.page == PK_PAGE_SETTINGS, "second press: page kept after long on second press");
    pk_app_button_edge(&app, 2750, PK_KEY_DOT, PK_EDGE_RELEASE);
    pk_app_tick(&app, 3000);
    expect(app.page == PK_PAGE_SETTINGS, "second press: release did not act as back");
}

static void test_page_authority(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_qualify_sample_t s;

    /* Frunk from MAIN that fails its check stays on MAIN. */
    pk_app_init(&app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&ad);
    ad.radio.write_once = capture_write;
    ad.radio.connect_once = fake_connect;
    ad.radio.disconnect = fake_disconnect;
    ad.radio.cancel = fake_cancel;
    ad.radio.has_session = fake_has_session;
    pk_ble_adapter_attach(&app, &ad);
    expect(add_and_select(&app, VIN_A) >= 0, "authority: add VIN_A");
    ad.connected_slot = app.vehicle;
    pk_app_set_connected(&app, true);
    pk_app_tick(&app, 1000);
    app.awake = true;
    app.page = PK_PAGE_MAIN;
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now), "authority: frunk freezes behind check");
    expect(app.has_deferred, "authority: frunk deferred");
    s = (pk_qualify_sample_t){
        .request_id = app.pending.id,
        .scope = app.pending.scope,
        .shift = PK_SHIFT_D,
        .sample_present = true,
        .sample = app.now,
        .clock_verified = true,
        .adapter_verified = true,
    };
    expect(!pk_app_qualification_result(&app, &s), "authority: D is not parked");
    expect(app.page == PK_PAGE_MAIN, "authority: failed check keeps MAIN");
    expect(!app.has_deferred, "authority: failed check drops the gesture");

    /* Status entered from MAIN returns to MAIN; entered from CARS returns to CARS. */
    app.moving = false;
    app.page = PK_PAGE_MAIN;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.page == PK_PAGE_STATUS, "authority: status opened");
    pk_app_exit_status(&app);
    expect(app.page == PK_PAGE_MAIN, "authority: status from MAIN returns MAIN");
    app.page = PK_PAGE_CARS;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    pk_app_exit_status(&app);
    expect(app.page == PK_PAGE_CARS, "authority: status from CARS returns CARS");

    /* Silent probe completion never moves the page. */
    app.page = PK_PAGE_SETTINGS;
    app.status_silent = true;
    app.status_step = 3;
    app.has_pending = true;
    app.pending.kind = PK_PENDING_STATUS;
    app.pending.id = app.serial;
    app.pending.scope = pk_app_scope(&app);
    app.pending.deadline = app.now + 5000;
    (void)pk_app_status_advance(&app);
    expect(app.page == PK_PAGE_SETTINGS, "authority: probe leaves the page");

    /* Takeover captures the interrupted page; nested presentation keeps it. */
    app.page = PK_PAGE_SETTINGS;
    pk_app_present_modal(&app, PK_PAGE_RESULT);
    expect(app.page == PK_PAGE_RESULT && app.modal_return_page == PK_PAGE_SETTINGS,
           "authority: modal captured SETTINGS");
    pk_app_present_modal(&app, PK_PAGE_RESULT);
    expect(app.modal_return_page == PK_PAGE_SETTINGS, "authority: nested modal keeps capture");
    pk_app_exit_wait(&app);
    expect(app.page == PK_PAGE_SETTINGS, "authority: dismiss returns to capture");

    /* A connection event repairs only the OFFLINE flow page. */
    app.page = PK_PAGE_SETTINGS;
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    expect(app.page == PK_PAGE_SETTINGS, "authority: connect does not move SETTINGS");
    app.page = PK_PAGE_OFFLINE;
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    expect(app.page == PK_PAGE_MAIN, "authority: connect completes the OFFLINE flow");
}

static void test_preset_vin_skips_softap(void)
{
    pk_app_t app;

    /* A preset VIN is already submitted: pair directly, no SoftAP portal. */
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    memcpy(app.vin, VIN_A, PK_VIN_BUF);
    app.has_vin = true;
    expect(pk_app_enter_vin(&app), "preset VIN enters pairing");
    expect(!app.ap, "preset VIN does not open the SoftAP portal");
    expect(app.page == PK_PAGE_PAIR && app.pair_step == 1,
           "preset VIN goes straight to PAIR step 1");
    expect(app.pair_deadline > app.now, "preset VIN sets the approval deadline");

    /* Preset VIN already in the store: add-car returns to the list, no re-pair. */
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    expect(pk_store_add_verified(&app.store, VIN_A, PK_STORE_CRASH_NONE),
           "store already holds VIN_A");
    memcpy(app.vin, VIN_A, PK_VIN_BUF);
    app.has_vin = true;
    expect(pk_app_enter_vin(&app), "stored preset VIN enter");
    expect(app.page == PK_PAGE_CARS && !app.ap,
           "stored preset VIN shows the car list without SoftAP");

    /* Without a preset the Wi-Fi VIN portal remains the entry path. */
    pk_app_init(&app, PK_VEHICLE_NONE, false);
    app.now = 1000;
    expect(pk_app_enter_vin(&app), "no preset enters VIN portal");
    expect(app.ap && app.page == PK_PAGE_VIN,
           "no preset opens the SoftAP VIN page");
}

static void test_sentry_idle_and_defrost_mode(void)
{
    pk_proto_carserver_resp_t r;

    /* ClosuresState.sentry_mode_state = Idle(2): Sentry is enabled but not
     * armed yet. It must count as "on" or the toggle keeps sending on. */
    {
        const uint8_t idle[] = { 0x0A, 0x00, 0x12, 0x07, 0x4A, 0x05,
                                 0x9A, 0x01, 0x02, 0x12, 0x00 };
        memset(&r, 0, sizeof(r));
        expect(pk_proto_parse_carserver_response(idle, sizeof(idle), &r),
               "sentry idle: parse");
        expect(r.has_closures_state && r.sentry_present && r.sentry_armed,
               "sentry idle(2) counts as enabled");
    }
    {
        const uint8_t off[] = { 0x0A, 0x00, 0x12, 0x07, 0x4A, 0x05,
                                0x9A, 0x01, 0x02, 0x0A, 0x00 };
        memset(&r, 0, sizeof(r));
        expect(pk_proto_parse_carserver_response(off, sizeof(off), &r),
               "sentry off: parse");
        expect(r.has_closures_state && r.sentry_present && !r.sentry_armed,
               "sentry off(1) counts as disabled");
    }

    /* ClimateState.DefrostMode = Max(3) is the real max-defrost state. */
    {
        const uint8_t max[] = { 0x0A, 0x00, 0x12, 0x07, 0x22, 0x05,
                                0x9A, 0x02, 0x02, 0x1A, 0x00 };
        memset(&r, 0, sizeof(r));
        expect(pk_proto_parse_carserver_response(max, sizeof(max), &r),
               "defrost max: parse");
        expect(r.has_climate_state && r.is_preconditioning_present &&
               r.is_preconditioning,
               "DefrostMode Max(3) counts as preconditioning on");
    }
    {
        const uint8_t off[] = { 0x0A, 0x00, 0x12, 0x07, 0x22, 0x05,
                                0x9A, 0x02, 0x02, 0x0A, 0x00 };
        memset(&r, 0, sizeof(r));
        expect(pk_proto_parse_carserver_response(off, sizeof(off), &r),
               "defrost off: parse");
        expect(r.has_climate_state && r.is_preconditioning_present &&
               !r.is_preconditioning,
               "DefrostMode Off(1) counts as preconditioning off");
    }
}

static void feed_adc_span(pk_app_t *app, pk_btn_adapter_t *btn,
                          pk_ms_t start_ms, pk_ms_t end_ms,
                          int mv, bool valid)
{
    pk_ms_t t;
    for (t = start_ms; t < end_ms; t += 5u) {
        (void)pk_btn_adapter_feed_mv(btn, t, mv, valid);
        (void)pk_app_tick(app, t);
    }
}

static void setup_connected_main_for_keys(pk_app_t *app, pk_ble_adapter_t *ad,
                                          pk_btn_adapter_t *btn)
{
    g_cap_writes = 0;
    pk_app_init(app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(ad);
    ad->radio.write_once = capture_write;
    ad->radio.connect_once = fake_connect;
    ad->radio.disconnect = fake_disconnect;
    ad->radio.cancel = fake_cancel;
    ad->radio.has_session = fake_has_session;
    pk_ble_adapter_attach(app, ad);
    expect(add_and_select(app, VIN_A) >= 0, "optE: add VIN_A");
    ad->connected_slot = app->vehicle;
    pk_app_set_connected(app, true);
    pk_app_tick(app, 1000);
    app->awake = true;
    app->page = PK_PAGE_MAIN;
    pk_btn_adapter_init(btn, app);
}

static void test_double_click_option_e_regressions(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_btn_adapter_t btn;

    /* 0. Pure 3-sample median filter rejects single-sample spikes and
     * preserves invalid when < 2 samples are valid. */
    {
        bool valid = false;
        expect(pk_btn_median3(303, 3300, 305, &valid) == 305 && valid,
               "optE: median3 rejects single high spike");
        expect(pk_btn_median3(-1, 303, 307, &valid) == 305 && valid,
               "optE: median3 averages 2 valid samples");
        expect(pk_btn_median3(-1, -1, 303, &valid) == -1 && !valid,
               "optE: median3 stays invalid when < 2 valid");
    }

    /* 1. Fast double-click: 60 ms and 90 ms physical release gaps must
     * produce a double-click (MAIN -> STATUS), never a single click, long
     * press, or vehicle command write. */
    {
        const pk_ms_t gaps[2] = { 60u, 90u };
        size_t i;
        for (i = 0; i < 2; i++) {
            pk_ms_t t = 1100u;
            pk_ms_t gap = gaps[i];
            setup_connected_main_for_keys(&app, &ad, &btn);
            feed_adc_span(&app, &btn, t, t + 100u, 303, true);          t += 100u;
            feed_adc_span(&app, &btn, t, t + gap, 2950, true);          t += gap;
            feed_adc_span(&app, &btn, t, t + 100u, 303, true);          t += 100u;
            feed_adc_span(&app, &btn, t, t + 500u, 2950, true);
            expect(app.page == PK_PAGE_STATUS && g_cap_writes == 1 &&
                   g_cap_frame.action == PK_ACTION_STATUS_READ,
                   "optE: fast double-click (60/90 ms gap) opens STATUS, no LOCK/SETTINGS");
        }
    }

    /* 2. Slow double-click: 220 ms and 260 ms physical release gaps on
     * SETTINGS must return to MAIN without splitting into two single clicks. */
    {
        const pk_ms_t gaps[2] = { 220u, 260u };
        size_t i;
        for (i = 0; i < 2; i++) {
            pk_ms_t t = 1100u;
            pk_ms_t gap = gaps[i];
            setup_connected_main_for_keys(&app, &ad, &btn);
            app.page = PK_PAGE_SETTINGS;
            app.focus = PK_SETTING_BRIGHTNESS;
            feed_adc_span(&app, &btn, t, t + 100u, 303, true);          t += 100u;
            feed_adc_span(&app, &btn, t, t + gap, 2950, true);          t += gap;
            feed_adc_span(&app, &btn, t, t + 100u, 303, true);          t += 100u;
            feed_adc_span(&app, &btn, t, t + 500u, 2950, true);
            expect(app.page == PK_PAGE_MAIN && !app.edit_open && g_cap_writes == 0,
                   "optE: slow double-click (220/260 ms gap) returns MAIN without click");
        }
    }

    /* 3. Main-loop stall (~70 ms) after first release: 5 ms samples buffered
     * in pk_btn_sample_ring_t are drained with their real timestamps before
     * pk_app_tick(late_now), so double-click still wins even when late_now > deadline. */
    {
        pk_btn_sample_ring_t ring;
        pk_btn_sample_t samp;
        pk_ms_t t;
        pk_ms_t late_now;

        setup_connected_main_for_keys(&app, &ad, &btn);
        app.page = PK_PAGE_SETTINGS;
        app.focus = PK_SETTING_BRIGHTNESS;
        pk_btn_ring_init(&ring);

        /* First press (1100..1180) and release (1180..1240 -> RELEASE at 1220, deadline=1520). */
        feed_adc_span(&app, &btn, 1100u, 1460u, 303, false); /* unused */
        setup_connected_main_for_keys(&app, &ad, &btn);
        app.page = PK_PAGE_SETTINGS;
        app.focus = PK_SETTING_BRIGHTNESS;
        feed_adc_span(&app, &btn, 1100u, 1180u, 303, true);
        feed_adc_span(&app, &btn, 1180u, 1460u, 2950, true);
        expect(app.keys.deadline_valid, "optE stall: deadline armed after first release");

        /* Main loop stalls for 70 ms (1460..1535) while 5 ms timer buffers
         * the second press starting at t=1460 (confirms at t=1500 <= deadline 1520). */
        for (t = 1460u; t <= 1535u; t += 5u) {
            (void)pk_btn_ring_push(&ring, t, 303, true);
        }
        late_now = 1535u; /* past deadline 1520 */
        while (pk_btn_ring_pop(&ring, &samp)) {
            (void)pk_btn_adapter_feed_mv(&btn, samp.ts_ms, samp.mv, samp.valid);
        }
        (void)pk_app_tick(&app, late_now);
        /* Release second press */
        feed_adc_span(&app, &btn, 1540u, 1620u, 2950, true);
        expect(app.page == PK_PAGE_MAIN && !app.edit_open && g_cap_writes == 0,
               "optE: 70 ms main-loop stall after release still recognizes double-click");
    }

    /* 4. ● release sweeping through ▼ window (600 mV for 15 ms) must not emit
     * PK_KEY_DOWN; an 80 ms cross-window glitch while holding ● is absorbed by
     * the 120 ms cross-window lock. */
    {
        setup_connected_main_for_keys(&app, &ad, &btn);
        app.page = PK_PAGE_SETTINGS;
        app.focus = PK_SETTING_BRIGHTNESS;
        feed_adc_span(&app, &btn, 1100u, 1200u, 303, true);
        /* 80 ms cross-window glitch into ▼ (600 mV) then back to ● (303 mV) */
        feed_adc_span(&app, &btn, 1200u, 1280u, 600, true);
        feed_adc_span(&app, &btn, 1280u, 1340u, 303, true);
        expect(app.focus == PK_SETTING_BRIGHTNESS && !app.side.active,
               "optE: 80 ms cross-window glitch absorbed by 120 ms lock");
        /* Release sweeping through ▼ for 15 ms then idle 2950 mV */
        feed_adc_span(&app, &btn, 1340u, 1355u, 600, true);
        feed_adc_span(&app, &btn, 1355u, 1450u, 2950, true);
        expect(app.focus == PK_SETTING_BRIGHTNESS && !app.side.active,
               "optE: release sweep through DOWN window emits no DOWN key");
    }

    /* 5. Invalid/failed ADC readings (60 ms) during ● hold must NOT release ●
     * early; 120 ms keep-alive holds the key and 500 ms long press still fires. */
    {
        setup_connected_main_for_keys(&app, &ad, &btn);
        feed_adc_span(&app, &btn, 1100u, 1300u, 303, true);
        feed_adc_span(&app, &btn, 1300u, 1360u, -1, false);
        expect(app.keys.down, "optE: 60 ms invalid ADC does not release DOT early");
        feed_adc_span(&app, &btn, 1360u, 1750u, 303, true);
        expect(app.page == PK_PAGE_SETTINGS,
               "optE: DOT long-press still fires after 60 ms invalid ADC glitch");
    }

    /* 6. Physical 430 ms hold (< 500 ms) must be a single-click, not inflated
     * by +80 ms into a 500 ms long-press. */
    {
        setup_connected_main_for_keys(&app, &ad, &btn);
        feed_adc_span(&app, &btn, 1100u, 1530u, 303, true);  /* 430 ms physical hold */
        feed_adc_span(&app, &btn, 1530u, 2000u, 2950, true); /* release + >300 ms */
        expect(app.page == PK_PAGE_MAIN && g_cap_writes == 1 &&
               g_cap_frame.action == PK_ACTION_LOCK,
               "optE: 430 ms physical hold commits single-click LOCK, not 500 ms SETTINGS");
    }

    /* 7. Background park-grant expiry during an active double-click window
     * must not clear deadline_valid. */
    {
        setup_connected_main_for_keys(&app, &ad, &btn);
        grant_park(&app);
        app.grant.until = 1300u; /* expires mid-double-click window */
        feed_adc_span(&app, &btn, 1100u, 1180u, 303, true);
        feed_adc_span(&app, &btn, 1180u, 1360u, 2950, true); /* grant expires at 1300 */
        expect(!app.has_grant && app.keys.deadline_valid,
               "optE: background grant expiry preserves active double-click deadline");
        feed_adc_span(&app, &btn, 1360u, 1440u, 303, true);
        feed_adc_span(&app, &btn, 1440u, 1520u, 2950, true);
        expect(app.page == PK_PAGE_STATUS,
               "optE: double-click completes across background grant expiry");
    }
}

int main(void)
{
    test_unwired_not_success();
    test_p16_reboot_releases_candidate();
    test_connect_none_invalid();
    test_protocol_crypto_and_wire();
    test_official_protocol_vectors();
    test_workflow_closure();
    test_nocar_double_click_via_adc_ladder();
    test_batch_1_to_3_regressions();
    test_deep_sleep_and_down_shortcut();
    test_power_role_arbiter();
    test_power_low_battery_sequence();
    test_power_simulator();
    test_four_pillar_closures();
    test_stable_branch_five_pillars();
    test_preset_vin_skips_softap();
    test_wake_then_and_toggle_ack();
    test_page_authority();
    test_vent_gesture_keeps_settings_page();
    test_second_press_long_never_returns();
    test_real_vehicle_ux_and_park_fixes();
    test_park_requires_explicit_p();
    test_session_and_fault_contract();
    test_direct_toggle_inline_feedback();
    test_check_before_act_and_lease_origin();
    test_check_before_act_from_side_and_settings();
    test_stale_park_grant_rechecks();
    test_stale_vehicle_sample_rejected();
    test_core_key_stays_on_vcsec();
    test_session_core_attribution_and_resync();
    test_session_info_clock_rollback_rejected();
    test_response_counter_reuse_rejected();
    test_session_fault_attribution();
    test_session_sync_timestamp();
    test_session_fault_gate();
    test_feedback_state_machine();
    test_feedback_classification();
    test_risk_contract();
    test_replay_is_lock_only();
    test_status_reads_one_section_per_request();
    test_plan_contract();
    test_status_fault25_skips_section();
    test_status_timeout_no_retry();
    test_status_out_of_order_rejected();
    test_status_mtu_paging();
    test_status_mtu_shrink_during_pump();
    test_status_learned_fault25();
    test_plaintext_vehicledata_parses_to_p();
    test_sentry_idle_and_defrost_mode();
    test_double_click_option_e_regressions();
    if (g_fail) {
        fprintf(stderr, "%d contract check(s) failed\n", g_fail);
        return 1;
    }
    puts("test_tesla_key_contract: ok");
    return 0;
}
