/*
 * S2 Security Pre-Gate + N01-N08 Unique Protocol Kernel Regression Suite
 *
 * Exercises Provider A (official vehicle-command 0a582da4 schema +
 * nanopb 0.4.9.1 generated codecs + bounded C11 thin session + real Mbed TLS
 * when compiled with -DPK_USE_MBEDTLS, or portable host fallback in CI).
 */

#include "pk_app.h"
#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_copy.h"
#include "pk_protocol.h"
#include "pk_session.h"
#include "pk_store.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "car_server.pb.h"
#include "signatures.pb.h"
#include "universal_message.pb.h"
#include "vcsec.pb.h"

#include <math.h>

/* The verify read has to be a real send before a packet may complete it. */
static pk_err_t kernel_verify_write(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    return PK_OK;
}

static bool kernel_verify_session(void *ctx, int slot, pk_domain_t domain)
{
    (void)ctx;
    (void)slot;
    (void)domain;
    return true;
}
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(PK_HOST_TEST_RNG_INJECT)
extern void pk_test_set_rng_fail(bool fail);
#endif

extern bool pk_proto_compute_cmd_metadata_hash(pk_proto_domain_t domain, const char *vin,
                                               const uint8_t epoch[16], uint32_t expires_at,
                                               uint32_t counter, uint32_t flags,
                                               uint8_t aad_out[32]);

extern bool pk_proto_compute_resp_metadata_hash(pk_proto_domain_t from_domain, const char *vin,
                                                uint32_t counter, uint32_t flags,
                                                const uint8_t req_id[17], uint32_t fault,
                                                uint8_t aad_out[32]);

static int g_pass;
static int g_fail;

static void check(bool cond, const char *id, const char *msg)
{
    if (cond) {
        g_pass++;
        printf("S2|%-8s|PASS|%s\n", id, msg);
    } else {
        g_fail++;
        fprintf(stderr, "S2|%-8s|FAIL|%s\n", id, msg);
    }
}

/* Official protocol.md public synthetic vectors (VIN = 5YJ30123456789ABC) */
static const char *const VEC_VIN = "5YJ30123456789ABC";

static const uint8_t VEC_CLIENT_PRIV[32] = {
    0x25, 0x38, 0xcd, 0xc2, 0x9a, 0x97, 0xc1, 0x9c,
    0x1e, 0x99, 0xa6, 0x37, 0xd6, 0xcf, 0x4f, 0x8c,
    0x97, 0x0c, 0x11, 0x8b, 0x56, 0xed, 0xe1, 0xe6,
    0x32, 0x3e, 0x6d, 0x16, 0x2c, 0x4b, 0x30, 0xdb
};
static const uint8_t VEC_CLIENT_PUB[65] = {
    0x04,
    0xb2, 0xb6, 0xbc, 0x68, 0xc2, 0xda, 0x06, 0x65,
    0xce, 0x65, 0x68, 0x15, 0x59, 0x49, 0x96, 0xc6,
    0x23, 0x94, 0xed, 0xd8, 0xbe, 0xa9, 0x05, 0xfe,
    0x78, 0x1a, 0x75, 0x4f, 0xe6, 0xa8, 0x45, 0xa7,
    0x14, 0x33, 0x09, 0x02, 0xf2, 0x25, 0xe9, 0x26,
    0x9d, 0x46, 0x6e, 0x05, 0xb3, 0x49, 0x98, 0x1f,
    0xda, 0x9d, 0x85, 0xcc, 0x23, 0xc6, 0xfb, 0x44,
    0x4a, 0xa7, 0x3b, 0x62, 0x91, 0x05, 0xdc, 0x6e
};
static const uint8_t VEC_VEH_PRIV[32] = {
    0x34, 0x4e, 0xe5, 0xb4, 0x66, 0xa7, 0xcf, 0x1e,
    0xeb, 0x12, 0xb6, 0xf5, 0x03, 0x31, 0xdb, 0x2e,
    0x5e, 0xc5, 0x83, 0x4e, 0xf5, 0xf4, 0xbe, 0xfc,
    0xfd, 0x8c, 0xbe, 0x55, 0xc2, 0x52, 0x8d, 0x70
};
static const uint8_t VEC_VEH_PUB[65] = {
    0x04,
    0xc7, 0xa1, 0xf4, 0x71, 0x38, 0x48, 0x6a, 0xa4,
    0x72, 0x99, 0x71, 0x49, 0x48, 0x78, 0xd3, 0x3b,
    0x1a, 0x24, 0xe3, 0x95, 0x71, 0xf7, 0x48, 0xa6,
    0xe1, 0x6c, 0x59, 0x55, 0xb3, 0xd8, 0x77, 0xd3,
    0xa6, 0xaa, 0xa0, 0xe9, 0x55, 0x16, 0x64, 0x74,
    0xaf, 0x5d, 0x32, 0xc4, 0x10, 0xf4, 0x39, 0xa2,
    0x23, 0x41, 0x37, 0xad, 0x1b, 0xb0, 0x85, 0xfd,
    0x4e, 0x88, 0x13, 0xc9, 0x58, 0xf1, 0x1d, 0x97
};
static const uint8_t VEC_K[16] = {
    0x1b, 0x2f, 0xce, 0x19, 0x96, 0x7b, 0x79, 0xdb,
    0x69, 0x6f, 0x90, 0x9c, 0xff, 0x89, 0xea, 0x9a
};
static const uint8_t VEC_EPOCH1[16] = {
    0x4c, 0x46, 0x3f, 0x9c, 0xc0, 0xd3, 0xd2, 0x69,
    0x06, 0xe9, 0x82, 0xed, 0x22, 0x4a, 0xdd, 0xe6
};
static const uint8_t VEC_EPOCH2[16] = {
    0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0x00, 0xfe, 0xdc
};
static const uint8_t ROUTE1[16] = {
    0x2c, 0x90, 0x7b, 0xd7, 0x6c, 0x64, 0x0d, 0x36,
    0x0b, 0x30, 0x27, 0xdc, 0x74, 0x04, 0xef, 0xde
};
static const uint8_t ROUTE2[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
};
static const uint8_t UUID1[16] = {
    0x15, 0x88, 0xd5, 0xa3, 0x0e, 0xab, 0xc6, 0xf8,
    0xfc, 0x9a, 0x95, 0x1b, 0x11, 0xf6, 0xfd, 0x11
};
static const uint8_t UUID2[16] = {
    0x58, 0x40, 0x65, 0x80, 0x52, 0x8b, 0x6a, 0x53,
    0x01, 0x39, 0x18, 0x00, 0xb4, 0xfe, 0x9b, 0x99
};
static const uint8_t UUID3[16] = {
    0x99, 0x40, 0x65, 0x80, 0x52, 0x8b, 0x6a, 0x53,
    0x01, 0x39, 0x18, 0x00, 0xb4, 0xfe, 0x9b, 0xaa
};
static const uint8_t NONCE1[12] = {
    0xdb, 0xf7, 0x94, 0x47, 0xfa, 0x15, 0x66, 0x74, 0xda, 0xe1, 0xca, 0xed
};

static size_t build_sim_session_info_wire(uint8_t *out, size_t cap,
                                          pk_proto_domain_t domain,
                                          const uint8_t *to_route,
                                          const uint8_t *req_uuid,
                                          const uint8_t challenge[16],
                                          const uint8_t veh_priv[32],
                                          const uint8_t veh_pub[65],
                                          const uint8_t epoch[16],
                                          uint32_t counter,
                                          uint32_t clock_time,
                                          bool bad_hmac,
                                          size_t tag_len)
{
    Signatures_SessionInfo si = Signatures_SessionInfo_init_zero;
    UniversalMessage_RoutableMessage rm = UniversalMessage_RoutableMessage_init_zero;
    uint8_t si_buf[128];
    pb_ostream_t s1 = pb_ostream_from_buffer(si_buf, sizeof(si_buf));
    uint8_t k[16], tag[32];

    si.counter = counter;
    si.publicKey.size = 65u;
    memcpy(si.publicKey.bytes, veh_pub, 65u);
    memcpy(si.epoch, epoch, 16u);
    si.clock_time = clock_time;
    if (!pb_encode(&s1, Signatures_SessionInfo_fields, &si)) return 0u;

    if (!pk_proto_ecdh_derive_session_key(veh_priv, VEC_CLIENT_PUB, k)) return 0u;
    pk_proto_session_info_msg_t sim = {
        .vin = VEC_VIN, .challenge = challenge, .challenge_len = 16u,
        .encoded_info = si_buf, .encoded_info_len = s1.bytes_written
    };
    if (!pk_proto_session_info_tag_compute(k, &sim, tag)) return 0u;
    if (bad_hmac) tag[0] ^= 0xFFu;

    rm.has_from_destination = true;
    rm.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    rm.from_destination.sub_destination.domain = (UniversalMessage_Domain)domain;
    if (to_route) {
        rm.has_to_destination = true;
        rm.to_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
        rm.to_destination.sub_destination.routing_address.size = 16u;
        memcpy(rm.to_destination.sub_destination.routing_address.bytes, to_route, 16u);
    }
    if (req_uuid) {
        rm.request_uuid.size = 16u;
        memcpy(rm.request_uuid.bytes, req_uuid, 16u);
    }
    rm.which_payload = UniversalMessage_RoutableMessage_session_info_tag;
    rm.payload.session_info.size = (pb_size_t)s1.bytes_written;
    memcpy(rm.payload.session_info.bytes, si_buf, s1.bytes_written);
    rm.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
    rm.sub_sigData.signature_data.which_sig_type = Signatures_SignatureData_session_info_tag_tag;
    rm.sub_sigData.signature_data.sig_type.session_info_tag.tag.size = (pb_size_t)tag_len;
    memcpy(rm.sub_sigData.signature_data.sig_type.session_info_tag.tag.bytes, tag, tag_len);

    pb_ostream_t s2 = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&s2, UniversalMessage_RoutableMessage_fields, &rm)) return 0u;
    return s2.bytes_written;
}

static size_t build_sim_vcsec_resp_wire_ex(uint8_t *out, size_t cap,
                                           pk_proto_domain_t domain,
                                           const uint8_t *to_route,
                                           const uint8_t *req_uuid,
                                           const char *aad_vin,
                                           const uint8_t req_id[17],
                                           uint32_t resp_counter,
                                           uint32_t aad_counter,
                                           bool encrypt,
                                           bool bad_tag,
                                           size_t tag_len,
                                           size_t nonce_len,
                                           bool empty_status)
{
    VCSEC_FromVCSECMessage fm = VCSEC_FromVCSECMessage_init_zero;
    UniversalMessage_RoutableMessage rm = UniversalMessage_RoutableMessage_init_zero;
    uint8_t pt[64], ct[64], aad[32], tag[16];
    uint8_t nonce[12] = { 1,2,3,4,5,6,7,8,9,10,11,(uint8_t)(resp_counter & 0xFFu) };

    fm.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
    if (!empty_status) {
        fm.sub_message.vehicleStatus.has_closureStatuses = true;
        fm.sub_message.vehicleStatus.vehicleLockState =
            VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED;
        fm.sub_message.vehicleStatus.vehicleSleepStatus =
            VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE;
    }
    pb_ostream_t ps = pb_ostream_from_buffer(pt, sizeof(pt));
    if (!pb_encode(&ps, VCSEC_FromVCSECMessage_fields, &fm)) return 0u;

    rm.has_from_destination = true;
    rm.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    rm.from_destination.sub_destination.domain = (UniversalMessage_Domain)domain;
    if (to_route) {
        rm.has_to_destination = true;
        rm.to_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
        rm.to_destination.sub_destination.routing_address.size = 16u;
        memcpy(rm.to_destination.sub_destination.routing_address.bytes, to_route, 16u);
    }
    if (req_uuid) {
        rm.request_uuid.size = 16u;
        memcpy(rm.request_uuid.bytes, req_uuid, 16u);
    }
    rm.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;

    if (!encrypt) {
        rm.payload.protobuf_message_as_bytes.size = (pb_size_t)ps.bytes_written;
        memcpy(rm.payload.protobuf_message_as_bytes.bytes, pt, ps.bytes_written);
    } else {
        if (!pk_proto_compute_resp_metadata_hash(domain, aad_vin ? aad_vin : VEC_VIN,
                                                 aad_counter, 0u, req_id, 0u, aad)) return 0u;
        if (!pk_proto_aes128_gcm_encrypt(VEC_K, nonce, aad, 32u, pt, ps.bytes_written, ct, tag)) return 0u;
        if (bad_tag) tag[0] ^= 0x55u;

        rm.payload.protobuf_message_as_bytes.size = (pb_size_t)ps.bytes_written;
        memcpy(rm.payload.protobuf_message_as_bytes.bytes, ct, ps.bytes_written);
        rm.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
        rm.sub_sigData.signature_data.which_sig_type =
            Signatures_SignatureData_AES_GCM_Response_data_tag;
        rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.nonce.size = (pb_size_t)nonce_len;
        memcpy(rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.nonce.bytes, nonce, nonce_len);
        rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.counter = resp_counter;
        rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag.size = (pb_size_t)tag_len;
        memcpy(rm.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag.bytes, tag, tag_len);
    }

    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&os, UniversalMessage_RoutableMessage_fields, &rm)) return 0u;
    return os.bytes_written;
}

static size_t build_sim_vcsec_resp_wire(uint8_t *out, size_t cap,
                                        pk_proto_domain_t domain,
                                        const uint8_t *to_route,
                                        const uint8_t *req_uuid,
                                        const char *aad_vin,
                                        const uint8_t req_id[17],
                                        uint32_t resp_counter,
                                        bool encrypt,
                                        bool bad_tag,
                                        size_t tag_len,
                                        size_t nonce_len,
                                        bool empty_status)
{
    return build_sim_vcsec_resp_wire_ex(out, cap, domain, to_route, req_uuid, aad_vin, req_id,
                                        resp_counter, resp_counter, encrypt, bad_tag,
                                        tag_len, nonce_len, empty_status);
}

/* ===== Security Pre-Gate 1: Bad SessionInfo HMAC / wrong challenge / truncated tag ===== */
static void test_pregate_1_bad_session_info_no_pollution(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_session_t snap;
    pk_proto_routable_t rmsg;
    uint8_t wire[384];
    size_t wlen;

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 1u);
    pk_proto_session_clear(&sess);

    /* Establish baseline valid session */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 10u, 2650u, false, 32u);
    check(pk_proto_parse_routable(wire, wlen, &rmsg), "PG1.0", "Baseline SessionInfo decoded");
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 1u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1100u, .session_now_ms = 1100u
    };
    check(pk_session_apply(&ctx, &ap, &sess) && sess.valid && sess.has_peer_pubkey,
          "PG1.1", "Baseline SessionInfo applied");
    snap = sess;

    /* Record an active handshake + active request pending state */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2, 1200u);
    pk_session_note_request_ex(&ctx, 0, 1u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 1200u);

    /* 1a. Bad HMAC tag */
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID2, UUID2, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH2, 99u, 9999u, true, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    ap.now_ms = 1250u;
    bool ok_bad_hmac = !pk_session_apply(&ctx, &ap, &sess) &&
                       memcmp(&sess, &snap, sizeof(sess)) == 0 &&
                       ctx.handshake[0].valid && ctx.request[0].valid;
    check(ok_bad_hmac, "PG1.2",
          "Bad SessionInfo HMAC rejected without polluting key/epoch/clock/counter/pending");

    /* 1b. Wrong challenge UUID */
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID2, UUID3, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH2, 99u, 9999u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    bool ok_bad_chal = !pk_session_apply(&ctx, &ap, &sess) &&
                       memcmp(&sess, &snap, sizeof(sess)) == 0 &&
                       ctx.handshake[0].valid && ctx.request[0].valid;
    check(ok_bad_chal, "PG1.3",
          "Wrong challenge SessionInfo rejected without polluting key/epoch/clock/counter/pending");

    /* 1c. Truncated 16-byte HMAC tag rejected */
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID2, UUID2, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH2, 99u, 9999u, false, 16u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    bool ok_trunc = !pk_session_apply(&ctx, &ap, &sess) &&
                    memcmp(&sess, &snap, sizeof(sess)) == 0 &&
                    ctx.handshake[0].valid;
    check(ok_trunc, "PG1.4",
          "Truncated 16B SessionInfo HMAC tag rejected without polluting session/pending");
}

/* ===== Security Pre-Gate 2: Same-epoch stale clock, new-epoch lower clock, old pending invalidation ===== */
static void test_pregate_2_epoch_clock_and_pending_invalidation(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    uint8_t wire[384], req_wire[384], req_id[17], plain[128];
    size_t wlen, plen = 0;
    pk_proto_rx_window_t win;
    pk_evidence_t ev = PK_EVIDENCE_UNKNOWN;

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 1u);
    pk_proto_session_clear(&sess);
    pk_proto_rx_window_init(&win);

    /* Establish epoch1, clock=2650, counter=10 */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 10u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 1u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1100u, .session_now_ms = 1100u
    };
    pk_session_apply(&ctx, &ap, &sess);
    uint32_t ep1_gen = sess.epoch_gen;

    /* 2a. Same-epoch stale clock (clock=2500 < 2650) does NOT pollute state */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2, 1200u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID2, UUID2, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 12u, 2500u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    ap.now_ms = 1250u;
    bool stale_rej = !pk_session_apply(&ctx, &ap, &sess);
    check(stale_rej && sess.clock_time == 2650u && sess.counter == 10u &&
          memcmp(sess.epoch, VEC_EPOCH1, 16u) == 0 && sess.epoch_gen == ep1_gen,
          "PG2.1", "Same-epoch stale clock sync does not pollute clock/epoch/counter");

    /* Issue a signed request under epoch1 */
    uint8_t inner[16];
    size_t ilen = pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, inner, sizeof(inner));
    pk_proto_signed_req_t sreq = {
        .route = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2 },
        .vin = VEC_VIN, .client_pubkey = VEC_CLIENT_PUB,
        .inner_payload = inner, .inner_len = ilen,
        .nonce = NONCE1, .now_ms = 1300u, .ttl_seconds = 5u
    };
    size_t rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 1u, ep1_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 1300u);
    check(rlen > 0u, "PG2.2", "Signed request built under epoch1");

    /* 2b. Vehicle reboots: NEW epoch2 with LOWER clock_time=5 (< 2650) MUST be accepted! */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID3, 1400u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID3, UUID3, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH2, 1u, 5u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    ap.now_ms = 1450u; ap.session_now_ms = 1450u;
    bool new_ep_ok = pk_session_apply(&ctx, &ap, &sess);
    check(new_ep_ok && memcmp(sess.epoch, VEC_EPOCH2, 16u) == 0 &&
          sess.clock_time == 5u && sess.epoch_gen > ep1_gen,
          "PG2.3", "New epoch with lower clock_time=5 accepted legitimately & epoch_gen bumped");

    /* 2c. Late response bound to old epoch1 pending txn MUST be rejected! */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, UUID2, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_rx_in_t rxin = {
        .slot = 0, .conn_gen = 1u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .now_ms = 1500u,
        .txn_active = true, .txn_slot = 0, .txn_conn_gen = 1u, .txn_epoch_gen = ep1_gen,
        .txn_deadline = 6000u, .txn_routing_addr = ROUTE1, .txn_uuid = UUID2,
        .txn_req_id = req_id, .rx_window = &win
    };
    bool late_rejected = !pk_session_accept_rx(&ctx, &rxin, plain, sizeof(plain), &plen, &ev);
    check(late_rejected && ev == PK_EVIDENCE_UNKNOWN && !ctx.request[0].valid,
          "PG2.4", "Old pending & late response from prior epoch cannot complete after epoch change");
}

/* ===== Security Pre-Gate 3: Production RNG entropy-source gate & fail-closed ===== */
static void test_pregate_3_rng_fail_closed(void)
{
    uint8_t buf[32], priv[32], pub[65], key[16];
    uint8_t seed[32];
    memset(buf, 0xAA, sizeof(buf));
    memset(priv, 0xBB, sizeof(priv));
    memset(pub, 0xCC, sizeof(pub));
    memset(key, 0xDD, sizeof(key));
    memset(seed, 0x11, sizeof(seed));

    /* Disable all RF entropy sources -> MUST fail closed and zeroize output */
    pk_proto_set_entropy_source(PK_ENTROPY_SRC_BLE | PK_ENTROPY_SRC_WIFI, false);
    bool ready_off = pk_proto_entropy_ready();
    bool rng_off = pk_proto_random_bytes(buf, sizeof(buf));
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0u) all_zero = false;
    }
    check(!ready_off && !rng_off && all_zero,
          "PG3.1", "RNG fails closed & zeroizes buffer when no RF entropy source is active");

#if defined(PK_USE_MBEDTLS)
    /* With entropy inactive, Mbed TLS P-256 keygen/ECDH MUST fail closed and zeroize outputs */
    bool kg_off = pk_proto_p256_keygen(priv, pub, seed);
    bool ecdh_off = pk_proto_ecdh_derive_session_key(VEC_CLIENT_PRIV, VEC_VEH_PUB, key);
    bool priv_z = true, pub_z = true, key_z = true;
    for (size_t i = 0; i < 32; i++) if (priv[i] != 0u) priv_z = false;
    for (size_t i = 0; i < 65; i++) if (pub[i] != 0u) pub_z = false;
    for (size_t i = 0; i < 16; i++) if (key[i] != 0u) key_z = false;
    check(!kg_off && !ecdh_off && priv_z && pub_z && key_z,
          "PG3.2", "Mbed TLS P-256 keygen/ECDH fail closed & zeroize outputs when RNG unavailable");
#else
    check(true, "PG3.2", "Portable host build (Mbed TLS RNG blinding tested under -DPK_USE_MBEDTLS)");
#endif

    /* Re-enable BLE RF entropy -> RNG & crypto succeed */
    pk_proto_set_entropy_source(PK_ENTROPY_SRC_BLE, true);
    bool rng_on = pk_proto_random_bytes(buf, sizeof(buf));
    bool ecdh_on = pk_proto_ecdh_derive_session_key(VEC_CLIENT_PRIV, VEC_VEH_PUB, key);
    check(pk_proto_entropy_ready() && rng_on && ecdh_on && memcmp(key, VEC_K, 16u) == 0,
          "PG3.3", "RNG & ECDH succeed once RF entropy source is active");

#if defined(PK_HOST_TEST_RNG_INJECT)
    pk_test_set_rng_fail(true);
    memset(buf, 0xEE, sizeof(buf));
    bool inj_fail = !pk_proto_random_bytes(buf, sizeof(buf));
    bool inj_z = true;
    for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0u) inj_z = false;
    pk_test_set_rng_fail(false);
    check(inj_fail && inj_z, "PG3.4", "Injected RNG failure fails closed & zeroizes output");
#endif
}

/* ===== Security Pre-Gate 4: First peer identity vs in-ctx continuity ===== */
static void test_pregate_4_peer_identity_vs_continuity(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    uint8_t wire[384];
    uint8_t atk_priv[32] = {0}, atk_pub[65];
    atk_priv[31] = 0x07u;
    pk_proto_p256_pubkey_from_priv(atk_priv, atk_pub);

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 1u);
    pk_proto_session_clear(&sess);
    check(!sess.has_peer_pubkey && !sess.valid,
          "PG4.1", "Fresh RAM session starts with no peer identity established");

    /* Establish first peer identity in this RAM ctx */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    size_t wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                              ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                              VEC_EPOCH1, 6u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 1u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1050u, .session_now_ms = 1050u
    };
    check(pk_session_apply(&ctx, &ap, &sess) && sess.has_peer_pubkey,
          "PG4.2", "First SessionInfo establishes in-ctx peer identity");

    /* Attempt to swap peer pubkey in same RAM ctx -> rejected, session key preserved */
    pk_session_note_handshake_ex(&ctx, 0, 1u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2, 1100u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID2, UUID2, atk_priv, atk_pub,
                                       VEC_EPOCH1, 7u, 2660u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    ap.now_ms = 1150u;
    check(!pk_session_apply(&ctx, &ap, &sess) &&
          memcmp(sess.vehicle_pubkey, VEC_VEH_PUB, 65u) == 0 &&
          memcmp(sess.session_key, VEC_K, 16u) == 0,
          "PG4.3", "In-ctx continuity rejects swapped peer pubkey & preserves session key");

    /* Explicit session clear resets in-ctx peer identity (explicitly NOT persistent NVS pinning) */
    pk_proto_session_clear(&sess);
    check(!sess.has_peer_pubkey && !sess.valid,
          "PG4.4", "pk_proto_session_clear resets RAM ctx (S2 does not alter NVS format)");
}

/* ===== N01-N08 Full Kernel Verification ===== */
static void test_n01_to_n08_kernel(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess_vcsec, sess_info;
    pk_proto_routable_t rmsg;
    pk_proto_rx_window_t win;
    uint8_t wire[512], req_wire[512], req_id[17], plain[256];
    size_t wlen, plen = 0;
    pk_evidence_t ev = PK_EVIDENCE_UNKNOWN;

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 10u);
    pk_proto_session_clear(&sess_vcsec);
    pk_proto_session_clear(&sess_info);
    pk_proto_rx_window_init(&win);

    /* Establish VCSEC session only (Infotainment remains isolated/invalid) */
    pk_session_note_handshake_ex(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 6u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1050u, .session_now_ms = 1050u
    };
    check(pk_session_apply(&ctx, &ap, &sess_vcsec) && sess_vcsec.valid && !sess_info.valid,
          "N01.0", "Dual-domain isolation: VCSEC session valid while Infotainment remains invalid");

    /* Build signed VCSEC GET_STATUS request */
    uint8_t inner[32];
    size_t ilen = pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, inner, sizeof(inner));
    pk_proto_signed_req_t sreq = {
        .route = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2 },
        .vin = VEC_VIN, .client_pubkey = VEC_CLIENT_PUB,
        .inner_payload = inner, .inner_len = ilen,
        .nonce = NONCE1, .now_ms = 2000u, .ttl_seconds = 5u
    };
    size_t rlen = pk_proto_build_signed_routable(&sreq, &sess_vcsec, req_id, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 10u, sess_vcsec.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2000u);
    check(rlen > 0u && sess_vcsec.counter == 7u, "N01.1", "Signed VCSEC request built with nanopb");

    pk_session_rx_in_t base = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess_vcsec, .rmsg = &rmsg, .now_ms = 2200u,
        .txn_active = true, .txn_slot = 0, .txn_conn_gen = 10u,
        .txn_epoch_gen = sess_vcsec.epoch_gen, .txn_deadline = 7000u,
        .txn_routing_addr = ROUTE1, .txn_uuid = UUID2,
        .txn_req_id = req_id, .rx_window = &win
    };

    /* N01: Missing route, wrong route, wrong conn_gen, expired deadline */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     NULL, UUID2, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(!pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev),
          "N01.2", "Missing to_routing_addr rejected");

    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE2, UUID2, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(!pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev),
          "N01.3", "Wrong to_routing_addr rejected");

    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_rx_in_t stale = base; stale.conn_gen = 9u;
    check(!pk_session_accept_rx(&ctx, &stale, plain, sizeof(plain), &plen, &ev),
          "N01.4", "Stale conn_gen rejected");
    pk_session_rx_in_t late = base; late.now_ms = 7000u;
    check(!pk_session_accept_rx(&ctx, &late, plain, sizeof(plain), &plen, &ev),
          "N01.5", "Expired deadline rejected");

    /* N02: Truncated GCM tag MUST NOT fall back to plaintext! */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, true, false, 8u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(!pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_UNKNOWN,
          "N02.1", "Truncated 8B GCM tag rejected fail-closed (never falls through to plaintext)");

    /* N02: Bad GCM tag with counter 0 MUST NOT poison window! */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, true, true, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(!pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev),
          "N02.2", "Tampered GCM tag rejected");

    /* N03: Authentic counter 0 STILL accepted after bad-tag counter 0 (auth-before-commit!) */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED,
          "N03.1", "Auth-before-commit: authentic counter 0 accepted after forged counter 0");

    /* N03: Out-of-order 2 -> 1 accepted, then A-B-A replay 0 rejected.
     * Official width 32: 100->68 is inside, 100->67 is outside. */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 2u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    bool r2 = pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev);
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 1u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    bool r1 = pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev);
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    bool r0_aba = !pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev);
    check(r2 && r1 && r0_aba, "N03.2", "Out-of-order 2->1 accepted; non-adjacent A-B-A replay 0 rejected");

    pk_proto_rx_window_t wbound;
    pk_proto_rx_window_init(&wbound);
    check(pk_proto_rx_window_accept(&wbound, 100u) &&
          pk_proto_rx_window_accept(&wbound, 68u) &&
          !pk_proto_rx_window_accept(&wbound, 67u) &&
          !pk_proto_rx_window_accept(&wbound, 68u),
          "N03.3", "Official width 32: 100->68 OK, 100->67 and dup 68 REJECTED");

    /* N03: TX counter rollover guard */
    sess_vcsec.counter = UINT32_MAX;
    check(pk_proto_build_signed_routable(&sreq, &sess_vcsec, req_id, req_wire, sizeof(req_wire)) == 0u &&
          sess_vcsec.counter == UINT32_MAX,
          "N03.4", "TX counter UINT32_MAX rollover refused without wrapping");

    /* N02: Legacy plaintext reply -> LEGACY_ATTRIBUTED, NEVER CRYPTO_AUTHENTICATED */
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id, 0u, false, false, 0u, 0u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    check(pk_session_accept_rx(&ctx, &base, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_LEGACY_ATTRIBUTED,
          "N02.3", "Plaintext reply classified LEGACY_ATTRIBUTED (never CRYPTO_AUTHENTICATED)");

    /* N02: Isolated pre-session pairing AddKey Step 3 */
    {
        VCSEC_FromVCSECMessage fm = VCSEC_FromVCSECMessage_init_zero;
        uint8_t pwire[64];
        pk_proto_vcsec_resp_t vr;
        fm.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
        fm.sub_message.commandStatus.operationStatus = VCSEC_OperationStatus_E_OPERATIONSTATUS_OK;
        fm.sub_message.commandStatus.which_sub_message = VCSEC_CommandStatus_whitelistOperationStatus_tag;
        fm.sub_message.commandStatus.sub_message.whitelistOperationStatus.whitelistOperationInformation =
            VCSEC_WhitelistOperation_information_E_WHITELISTOPERATION_INFORMATION_NONE;
        pb_ostream_t ps = pb_ostream_from_buffer(pwire, sizeof(pwire));
        pb_encode(&ps, VCSEC_FromVCSECMessage_fields, &fm);

        bool rej_when_session = !pk_session_accept_pre_session_pair(true, true, pwire, ps.bytes_written, &vr, &ev);
        bool rej_when_not_step3 = !pk_session_accept_pre_session_pair(false, false, pwire, ps.bytes_written, &vr, &ev);
        bool ok_step3 = pk_session_accept_pre_session_pair(true, false, pwire, ps.bytes_written, &vr, &ev) &&
                        ev == PK_EVIDENCE_LEGACY_ATTRIBUTED && vr.has_whitelist_status;
        check(rej_when_session && rej_when_not_step3 && ok_step3,
              "N02.4", "Pre-session pairing entry isolated strictly to pair_step3 && !vcsec_session");
    }

    /* N04: Official nanopb CarServer DriveState presence vs invalid + unrelated section preservation (S2 §2D) */
    {
        CarServer_Response cr = CarServer_Response_init_zero;
        uint8_t cwire[128];
        pk_proto_carserver_resp_t out;

        cr.which_response_msg = CarServer_Response_vehicleData_tag;
        cr.response_msg.vehicleData.has_drive_state = true;
        cr.response_msg.vehicleData.drive_state.has_shift_state = true;
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_P_tag;
        /* Include unrelated display section (ChargeState SOC=77) in same frame. */
        cr.response_msg.vehicleData.has_charge_state = true;
        cr.response_msg.vehicleData.charge_state.which_optional_battery_level =
            CarServer_ChargeState_battery_level_tag;
        cr.response_msg.vehicleData.charge_state.optional_battery_level.battery_level = 77;
        pb_ostream_t s1 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s1, CarServer_Response_fields, &cr);
        bool ok_unset = pk_proto_parse_carserver_response(cwire, s1.bytes_written, &out) &&
                        out.has_drive_state && out.shift == PK_SHIFT_P &&
                        !out.speed_present && !out.speed_float_present &&
                        !out.speed_float_invalid && !out.speed_invalid &&
                        out.battery_level_present && out.battery_level == 77;

        /* Finite 0.0f: present=true, invalid=false, mph=0 */
        cr.response_msg.vehicleData.drive_state.which_optional_speed_float = CarServer_DriveState_speed_float_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = 0.0f;
        pb_ostream_t s4 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s4, CarServer_Response_fields, &cr);
        bool ok_zero = pk_proto_parse_carserver_response(cwire, s4.bytes_written, &out) &&
                       out.speed_float_present && !out.speed_float_invalid &&
                       !out.speed_invalid && out.speed_float_mph == 0;

        check(ok_unset && ok_zero,
              "N04.1a", "DriveState presence: unset vs finite 0.0f distinct & valid");

        /* Non-finite (+Inf / NaN) combined with speed=0: preserves present=true AND
         * invalid=true so it cannot qualify as Park, while unrelated SOC=77 stays intact. */
        cr.response_msg.vehicleData.drive_state.which_optional_speed = CarServer_DriveState_speed_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 0;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = INFINITY;
        pb_ostream_t s2 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s2, CarServer_Response_fields, &cr);
        bool ok_inf = pk_proto_parse_carserver_response(cwire, s2.bytes_written, &out) &&
                      out.speed_present && out.speed_mph == 0 &&
                      out.speed_float_present && out.speed_float_invalid && out.speed_invalid &&
                      out.battery_level_present && out.battery_level == 77;

        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = NAN;
        pb_ostream_t s3 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s3, CarServer_Response_fields, &cr);
        bool ok_nan = pk_proto_parse_carserver_response(cwire, s3.bytes_written, &out) &&
                      out.speed_present && out.speed_mph == 0 &&
                      out.speed_float_present && out.speed_float_invalid && out.speed_invalid &&
                      out.battery_level_present && out.battery_level == 77;

        /* Conflicting speed=0 and speed_float=5.0f marks speed_invalid=true */
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = 5.0f;
        pb_ostream_t s5 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s5, CarServer_Response_fields, &cr);
        bool ok_conflict = pk_proto_parse_carserver_response(cwire, s5.bytes_written, &out) &&
                           out.speed_present && out.speed_float_present &&
                           !out.speed_float_invalid && out.speed_invalid;

        check(ok_inf && ok_nan && ok_conflict,
              "N04.1b", "DriveState invalid: speed=0 + NaN/+Inf & conflicting speed mark invalid while preserving SOC");

        /* N04.1c: illegal speed must not drop an independent gear or a finite speed. */
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_D_tag;
        cr.response_msg.vehicleData.drive_state.which_optional_speed = CarServer_DriveState_speed_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed.speed = 5;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = NAN;
        pb_ostream_t s6 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s6, CarServer_Response_fields, &cr);
        bool ok_d_nan = pk_proto_parse_carserver_response(cwire, s6.bytes_written, &out) &&
                        out.shift == PK_SHIFT_D && out.speed_present && out.speed_mph == 5 &&
                        out.speed_float_present && out.speed_float_invalid && out.speed_invalid;
        cr.response_msg.vehicleData.drive_state.which_optional_speed = 0;
        cr.response_msg.vehicleData.drive_state.shift_state.which_type = CarServer_ShiftState_R_tag;
        cr.response_msg.vehicleData.drive_state.optional_speed_float.speed_float = INFINITY;
        pb_ostream_t s7 = pb_ostream_from_buffer(cwire, sizeof(cwire));
        pb_encode(&s7, CarServer_Response_fields, &cr);
        bool ok_r_inf = pk_proto_parse_carserver_response(cwire, s7.bytes_written, &out) &&
                        out.shift == PK_SHIFT_R && !out.speed_present &&
                        out.speed_float_present && out.speed_float_invalid && out.speed_invalid;
        check(ok_d_nan && ok_r_inf,
              "N04.1c", "DriveState keeps D/R and a finite speed beside NaN/Inf");

        /* Empty VehicleStatus{} does NOT fabricate closureStatuses */
        wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                         ROUTE1, NULL, VEC_VIN, req_id, 0u, false, false, 0u, 0u, true);
        pk_proto_parse_routable(wire, wlen, &rmsg);
        pk_proto_vcsec_resp_t vempty;
        check(pk_proto_parse_vcsec_response(rmsg.payload_bytes, rmsg.payload_len, &vempty) &&
              vempty.has_vehicle_status && !vempty.has_closure_statuses,
              "N04.2", "Empty VehicleStatus{} keeps has_closure_statuses=false");
    }

    /* N05: Coalesced frames & trailing prefix preservation via pk_rx_framer_feed_ex */
    {
        pk_rx_framer_t f;
        uint8_t w1[16], w2[16], combo[32];
        const uint8_t m1[4] = { 1, 2, 3, 4 };
        const uint8_t m2[5] = { 9, 8, 7, 6, 5 };
        size_t l1 = pk_proto_frame_wrap(m1, sizeof(m1), w1, sizeof(w1));
        size_t l2 = pk_proto_frame_wrap(m2, sizeof(m2), w2, sizeof(w2));
        memcpy(combo, w1, l1);
        memcpy(combo + l1, w2, 3u); /* 2B header + 1B payload of frame 2 */

        pk_rx_framer_init(&f);
        size_t c1 = 0, c2 = 0, c3 = 0;
        int r1 = pk_rx_framer_feed_ex(&f, combo, l1 + 3u, 100u, &c1);
        bool ok1 = (r1 == 1 && f.expected_len == 4u && memcmp(f.buf, m1, 4u) == 0);
        int r2 = pk_rx_framer_feed_ex(&f, combo + c1, (l1 + 3u) - c1, 100u, &c2);
        bool ok2 = (r2 == 0 && c1 + c2 == l1 + 3u);
        int r3 = pk_rx_framer_feed_ex(&f, w2 + 3u, l2 - 3u, 150u, &c3);
        bool ok3 = (r3 == 1 && f.expected_len == 5u && memcmp(f.buf, m2, 5u) == 0);
        check(ok1 && ok2 && ok3,
              "N05.1", "Streaming framer extracts coalesced frame & retains trailing prefix");
    }

    /* N06: GATT fragmentation, total budget, stale callback token, queue overflow */
    {
        pk_gatt_tx_t tx;
        size_t cl = 0;
        bool matched = false;
        pk_gatt_tx_init(&tx, 1u, 23u);
        check(pk_gatt_tx_chunk_size(&tx) == 20u &&
              pk_gatt_tx_begin(&tx, 45u, 1000u, 1100u) &&
              pk_gatt_tx_step(&tx, 1010u, true, 0, &cl) == PK_OK && cl == 20u &&
              pk_gatt_tx_step(&tx, 1100u, true, 0, &cl) == PK_ERR_EXPIRED &&
              tx.last_outcome == PK_TX_SENT_UNKNOWN,
              "N06.1", "MTU=23->20B fallback & total budget expiry marks SentUnknown");

        pk_gatt_tx_init(&tx, 2u, 23u);
        pk_gatt_tx_begin(&tx, 20u, 2000u, 5000u); /* write_seq = 1 */
        pk_gatt_tx_step(&tx, 2010u, true, 0, &cl);
        pk_gatt_tx_enqueue_cb(&tx, 1u, 1u, 0); /* stale conn_gen */
        pk_gatt_tx_enqueue_cb(&tx, 2u, 0u, 0); /* stale write_seq */
        pk_gatt_tx_enqueue_cb(&tx, 2u, 1u, 0); /* current */
        bool m1 = true, m2 = true, m3 = false;
        pk_gatt_tx_poll_cb(&tx, 2020u, &m1);
        pk_gatt_tx_poll_cb(&tx, 2020u, &m2);
        pk_gatt_tx_poll_cb(&tx, 2020u, &m3);
        check(!m1 && !m2 && m3 && tx.last_outcome == PK_TX_DELIVERED,
              "N06.2", "Stale (conn_gen, write_seq) callbacks ignored; matching token delivers");

        pk_gatt_tx_init(&tx, 3u, 23u);
        for (uint32_t i = 0; i < PK_GATT_CB_QUEUE_CAP; i++) {
            pk_gatt_tx_enqueue_cb(&tx, 3u, i + 1u, 0);
        }
        check(pk_gatt_tx_enqueue_cb(&tx, 3u, 99u, 0) == PK_ERR_BUSY && tx.cb_overflow_drops == 1u,
              "N06.3", "GATT callback queue overflow fails explicitly with drop count");
        (void)matched;
    }

    /* N07 / N08: Legacy plaintext (authenticated=false) NEVER confirms lock verify or Park grant */
    {
        pk_app_t app;
        pk_ble_adapter_t ad;
        pk_app_init(&app, PK_VEHICLE_NONE, false);
        pk_store_add_verified(&app.store, VEC_VIN, PK_STORE_CRASH_NONE);
        pk_app_select_vehicle(&app, 0);
        pk_ble_adapter_init(&ad);
        ad.radio.write_once = kernel_verify_write;
        ad.radio.has_session = kernel_verify_session;
        pk_ble_adapter_attach(&app, &ad);
        pk_app_set_connected(&app, true);
        pk_app_tick(&app, 1000u);

        /* N08: start lock verify -> feed legacy plaintext (authenticated=false) -> rejected! */
        pk_app_start_lock_verification(&app, PK_CMD_RESULT_ACCEPTED);
        pk_lock_packet_t lpkt = {
            .request_id = app.pending.id,
            .authenticated = false, /* PK_EVIDENCE_LEGACY_ATTRIBUTED */
            .correlated = true,
            .adapter_verified = false,
            .lock = PK_LOCK_READ_LOCKED,
            .closures = NULL,
            .closures_complete = false,
            .complete = true,
        };
        check(!pk_app_lock_packet(&app, &lpkt),
              "N08.1", "Lock verification rejects Legacy plaintext (authenticated=false)");

        /* Authenticated packet with missing closures -> appends CLOSURES_SOME_UNKNOWN (never false all-closed) */
        lpkt.authenticated = true;
        lpkt.adapter_verified = true;
        check(pk_app_lock_packet(&app, &lpkt) &&
              strstr(app.message_extra, pk_copy(PK_MSG_CLOSURES_SOME_UNKNOWN)) != NULL,
              "N08.2", "Lock verification with empty closureStatuses flags unknown closures (no false all-closed)");
    }
}

/* Signed responses bind to a noted req_id. A missing note must not accept
 * through the shared domain window, or the next request's counter 0 is lost. */
static void test_req_id_required_no_domain_fallback(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    pk_proto_rx_window_t private_win;
    pk_proto_rx_window_t fresh;
    uint8_t wire[512], req_wire[512];
    uint8_t req_id_a[17], req_id_b[17], req_id_c[17], plain[256], inner[32];
    uint8_t nonce_b[12] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
    uint8_t nonce_c[12] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
    size_t wlen, plen = 0, ilen, rlen;
    pk_evidence_t ev = PK_EVIDENCE_UNKNOWN;
    pk_proto_rx_window_t *dom;
    pk_proto_rx_window_t *win_b;
    pk_proto_rx_window_t *win_c;

    check(PK_PROTO_RX_WINDOW_SIZE == 32u,
          "RID.0", "anti-replay window width stays 32");

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 10u);
    pk_proto_session_clear(&sess);
    pk_proto_rx_window_init(&private_win);
    pk_proto_rx_window_init(&fresh);
    memset(req_id_a, 0, sizeof(req_id_a));
    memset(req_id_b, 0, sizeof(req_id_b));
    memset(req_id_c, 0, sizeof(req_id_c));

    pk_session_note_handshake_ex(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 6u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1050u, .session_now_ms = 1050u
    };
    check(pk_session_apply(&ctx, &ap, &sess) && sess.valid,
          "RID.1", "session ready for req_id tests");

    ilen = pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, inner, sizeof(inner));
    pk_proto_signed_req_t sreq = {
        .route = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2 },
        .vin = VEC_VIN, .client_pubkey = VEC_CLIENT_PUB,
        .inner_payload = inner, .inner_len = ilen,
        .nonce = NONCE1, .now_ms = 2000u, .ttl_seconds = 5u
    };
    rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id_a, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 10u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2000u);
    dom = pk_session_rx_window(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY);
    check(rlen > 0u &&
          req_id_a[0] == (uint8_t)Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_PERSONALIZED &&
          pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_a) == NULL &&
          pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, NULL) == NULL &&
          dom != NULL && memcmp(dom, &fresh, sizeof(fresh)) == 0,
          "RID.2", "signed request identity is absent until note_req_id; no domain-window fallback");

    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id_a, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_rx_in_t rx = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .now_ms = 2200u,
        .txn_active = true, .txn_slot = 0, .txn_conn_gen = 10u,
        .txn_epoch_gen = sess.epoch_gen, .txn_deadline = 7000u,
        .txn_routing_addr = ROUTE1, .txn_uuid = UUID2,
        .txn_req_id = req_id_a,
        .rx_window = pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_a)
    };
    check(!pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_UNKNOWN && memcmp(dom, &fresh, sizeof(fresh)) == 0,
          "RID.3", "unnoted signed response is rejected and does not consume the domain window");

    rx.rx_window = &private_win;
    check(pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED && memcmp(dom, &fresh, sizeof(fresh)) == 0,
          "RID.4", "the same counter 0 is authentic; rejection was the missing note, not the tag");

    {
        pk_session_obs_in_t obs = {
            .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
            .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .req_id = req_id_a
        };
        plen = 0;
        ev = PK_EVIDENCE_UNKNOWN;
        check(!pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
              memcmp(dom, &fresh, sizeof(fresh)) == 0,
              "RID.5", "unnoted observation does not accept via the shared domain window");
    }

    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id_a, 0u, false, false, 0u, 0u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    rx.rmsg = &rmsg;
    rx.rx_window = NULL;
    check(pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_LEGACY_ATTRIBUTED,
          "RID.6", "unsigned plaintext still accepts without a noted req_id");

    sreq.nonce = nonce_b;
    rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id_b, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 10u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2100u);
    pk_session_note_req_id(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_b);
    win_b = pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_b);
    check(rlen > 0u &&
          req_id_b[0] == (uint8_t)Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_PERSONALIZED &&
          win_b != NULL && win_b != dom,
          "RID.7", "noting the signed request registers its own window, not the domain window");

    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id_b, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    rx.rmsg = &rmsg;
    rx.txn_req_id = req_id_b;
    rx.rx_window = win_b;
    check(pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED && memcmp(dom, &fresh, sizeof(fresh)) == 0,
          "RID.8", "noted request accepts counter 0 on its own window; domain window stays unused");
    check(!pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev),
          "RID.9", "replay of counter 0 on the same noted request is rejected");

    sreq.nonce = nonce_c;
    rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id_c, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 10u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2300u);
    pk_session_note_req_id(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_c);
    win_c = pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id_c);
    wlen = build_sim_vcsec_resp_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                     ROUTE1, NULL, VEC_VIN, req_id_c, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    rx.rmsg = &rmsg;
    rx.txn_req_id = req_id_c;
    rx.rx_window = win_c;
    check(rlen > 0u && win_c != NULL && win_c != dom && win_c != win_b &&
          pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED && memcmp(dom, &fresh, sizeof(fresh)) == 0,
          "RID.10", "next request counter 0 is kept because the missing note did not burn the domain window");
}

/* Official A12: response AAD TAG_COUNTER is AES_GCM_Response_data.counter.
 * The GCM tag is checked in full before that counter enters the window. */
static void test_response_aad_counter_before_window(void)
{
    pk_session_ctx_t ctx;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    pk_proto_rx_window_t *win;
    pk_proto_rx_window_t fresh;
    pk_proto_rx_window_t snap;
    pk_session_rx_in_t rx;
    pk_session_obs_in_t obs;
    uint8_t wire[512];
    uint8_t req_wire[512];
    uint8_t req_id[17];
    uint8_t plain[256];
    uint8_t inner[32];
    size_t wlen;
    size_t plen = 0;
    size_t ilen;
    size_t rlen;
    pk_evidence_t ev = PK_EVIDENCE_UNKNOWN;
    uint32_t request_counter;
    const uint32_t response_counter = 0u;

    pk_session_init(&ctx);
    pk_session_set_conn_gen(&ctx, 10u);
    pk_proto_session_clear(&sess);
    pk_proto_rx_window_init(&fresh);
    memset(req_id, 0, sizeof(req_id));

    pk_session_note_handshake_ex(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 6u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1050u, .session_now_ms = 1050u
    };
    check(wlen > 0u && pk_session_apply(&ctx, &ap, &sess) && sess.valid && sess.counter == 6u,
          "A12.0", "session starts at command counter 6");

    ilen = pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, inner, sizeof(inner));
    pk_proto_signed_req_t sreq = {
        .route = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2 },
        .vin = VEC_VIN, .client_pubkey = VEC_CLIENT_PUB,
        .inner_payload = inner, .inner_len = ilen,
        .nonce = NONCE1, .now_ms = 2000u, .ttl_seconds = 5u
    };
    rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id, req_wire, sizeof(req_wire));
    request_counter = sess.counter;
    pk_session_note_request_ex(&ctx, 0, 10u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2000u);
    pk_session_note_req_id(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id);
    win = pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id);
    check(rlen > 0u && request_counter == 7u && request_counter != response_counter &&
          win != NULL && memcmp(win, &fresh, sizeof(fresh)) == 0,
          "A12.1", "request counter 7 is not the response counter; noted window is unused");

    rx = (pk_session_rx_in_t){
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .now_ms = 2200u,
        .txn_active = true, .txn_slot = 0, .txn_conn_gen = 10u,
        .txn_epoch_gen = sess.epoch_gen, .txn_deadline = 7000u,
        .txn_routing_addr = ROUTE1, .txn_uuid = UUID2,
        .txn_req_id = req_id, .rx_window = win
    };

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        response_counter, request_counter,
                                        true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          rmsg.has_aes_gcm_resp && rmsg.resp_counter == response_counter &&
          !pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_UNKNOWN &&
          win != NULL && memcmp(win, &fresh, sizeof(fresh)) == 0 &&
          sess.counter == request_counter,
          "A12.2", "response AAD built with the request counter is rejected and does not enter the window");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        response_counter, response_counter,
                                        true, true, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          !pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_UNKNOWN &&
          win != NULL && memcmp(win, &fresh, sizeof(fresh)) == 0 &&
          sess.counter == request_counter,
          "A12.3", "failed GCM tag does not enter or consume the anti-replay window");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        response_counter, response_counter,
                                        true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED && plen > 0u &&
          win != NULL && win->used && win->counter == response_counter &&
          win->history == 0ULL &&
          sess.counter == request_counter,
          "A12.4", "same response counter is accepted when AAD uses that counter and the tag is valid");

    if (win != NULL) {
        memcpy(&snap, win, sizeof(snap));
    }
    check(win != NULL &&
          !pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          memcmp(win, &snap, sizeof(snap)) == 0,
          "A12.5", "replay of the accepted response counter is rejected and leaves the window unchanged");

    obs = (pk_session_obs_in_t){
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .req_id = req_id
    };
    if (win != NULL) {
        memcpy(&snap, win, sizeof(snap));
    }
    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        9u, request_counter,
                                        true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          rmsg.resp_counter == 9u &&
          !pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          win != NULL && memcmp(win, &snap, sizeof(snap)) == 0,
          "A12.6", "observation rejects an AAD that uses the request counter and does not move the window");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        9u, 9u, true, true, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          !pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          win != NULL && memcmp(win, &snap, sizeof(snap)) == 0,
          "A12.7", "observation does not consume the window when the GCM tag fails");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        9u, 9u, true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED &&
          win != NULL && win->used && win->counter == 9u &&
          sess.counter == request_counter,
          "A12.8", "observation accepts counter 9 after the failed tag when AAD uses that counter");
}

/* Issue #390 / P-05: after connect, 31- and 41-byte passive authentication
 * frames that do not match this endpoint's routing address are dropped and
 * must not enter the anti-replay window. A same-sized frame that does match
 * is still eligible. */
static void test_passive_auth_unmatched_not_in_window(void)
{
    pk_session_ctx_t ctx;
    pk_session_ctx_t empty;
    pk_proto_session_t sess;
    pk_proto_routable_t rmsg;
    pk_proto_routable_t raw;
    pk_proto_rx_window_t *win;
    pk_proto_rx_window_t snap;
    pk_session_rx_in_t rx;
    pk_session_obs_in_t obs;
    uint8_t wire[512];
    uint8_t req_wire[512];
    uint8_t req_id[17];
    uint8_t plain[256];
    uint8_t inner[32];
    uint8_t frame31[31];
    uint8_t frame41[41];
    size_t wlen;
    size_t plen = 0;
    size_t ilen;
    size_t rlen;
    pk_evidence_t ev = PK_EVIDENCE_UNKNOWN;
    unsigned i;

    pk_session_init(&ctx);
    pk_session_init(&empty);
    pk_session_set_conn_gen(&ctx, 10u);
    pk_proto_session_clear(&sess);
    memset(req_id, 0, sizeof(req_id));

    pk_session_note_handshake_ex(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID1, 1000u);
    wlen = build_sim_session_info_wire(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                       ROUTE1, UUID1, UUID1, VEC_VEH_PRIV, VEC_VEH_PUB,
                                       VEC_EPOCH1, 6u, 2650u, false, 32u);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    pk_session_apply_t ap = {
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .privkey = VEC_CLIENT_PRIV, .rmsg = &rmsg,
        .now_ms = 1050u, .session_now_ms = 1050u
    };
    check(wlen > 0u && pk_session_apply(&ctx, &ap, &sess) && sess.valid,
          "P05.0", "session ready for passive-auth frame filter");

    ilen = pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, inner, sizeof(inner));
    pk_proto_signed_req_t sreq = {
        .route = { PK_PROTO_DOMAIN_VEHICLE_SECURITY, ROUTE1, UUID2 },
        .vin = VEC_VIN, .client_pubkey = VEC_CLIENT_PUB,
        .inner_payload = inner, .inner_len = ilen,
        .nonce = NONCE1, .now_ms = 2000u, .ttl_seconds = 5u
    };
    rlen = pk_proto_build_signed_routable(&sreq, &sess, req_id, req_wire, sizeof(req_wire));
    pk_session_note_request_ex(&ctx, 0, 10u, sess.epoch_gen, PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                               ROUTE1, UUID2, true, 2000u);
    pk_session_note_req_id(&ctx, 0, 10u, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id);
    win = pk_session_rx_window_for_req(&ctx, PK_PROTO_DOMAIN_VEHICLE_SECURITY, req_id);
    check(rlen > 0u && win != NULL,
          "P05.1", "endpoint routing address ROUTE1 is recorded with its window");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        0u, 0u, true, false, 16u, 12u, false);
    pk_proto_parse_routable(wire, wlen, &rmsg);
    rx = (pk_session_rx_in_t){
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .now_ms = 2200u,
        .txn_active = true, .txn_slot = 0, .txn_conn_gen = 10u,
        .txn_epoch_gen = sess.epoch_gen, .txn_deadline = 7000u,
        .txn_routing_addr = ROUTE1, .txn_uuid = UUID2,
        .txn_req_id = req_id, .rx_window = win, .frame_len = 0u
    };
    check(wlen > 0u && pk_session_accept_rx(&ctx, &rx, plain, sizeof(plain), &plen, &ev) &&
          win->used && win->counter == 0u,
          "P05.2", "matching response still enters the window");
    memcpy(&snap, win, sizeof(snap));

    /* Same decryptable body, addressed to a different endpoint, declared as
     * the 31-byte passive-auth size. Observation would otherwise commit. */
    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE2, NULL, VEC_VIN, req_id,
                                        3u, 3u, true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          rmsg.has_to_routing_addr &&
          pk_session_passive_auth_unmatched(&ctx, 31u, &rmsg, NULL) &&
          pk_session_passive_auth_unmatched(&ctx, 31u, &rmsg, ROUTE1),
          "P05.3", "31-byte frame with a foreign routing address is unmatched");
    obs = (pk_session_obs_in_t){
        .slot = 0, .conn_gen = 10u, .domain = PK_PROTO_DOMAIN_VEHICLE_SECURITY,
        .vin = VEC_VIN, .sess = &sess, .rmsg = &rmsg, .req_id = req_id,
        .frame_len = 31u, .endpoint_routing = ROUTE1
    };
    plen = 0;
    ev = PK_EVIDENCE_UNKNOWN;
    check(!pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          memcmp(win, &snap, sizeof(snap)) == 0,
          "P05.4", "unmatched 31-byte passive auth is dropped and does not enter the window");

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE1, NULL, VEC_VIN, req_id,
                                        3u, 3u, true, false, 16u, 12u, false);
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          !pk_session_passive_auth_unmatched(&ctx, 31u, &rmsg, ROUTE1) &&
          pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          ev == PK_EVIDENCE_CRYPTO_AUTHENTICATED &&
          win->used && win->counter == 3u,
          "P05.5", "31-byte frame that matches this endpoint still enters the window");
    memcpy(&snap, win, sizeof(snap));

    wlen = build_sim_vcsec_resp_wire_ex(wire, sizeof(wire), PK_PROTO_DOMAIN_VEHICLE_SECURITY,
                                        ROUTE2, NULL, VEC_VIN, req_id,
                                        4u, 4u, true, false, 16u, 12u, false);
    obs.frame_len = 41u;
    obs.endpoint_routing = NULL;
    check(wlen > 0u && pk_proto_parse_routable(wire, wlen, &rmsg) &&
          pk_session_passive_auth_unmatched(&ctx, 41u, &rmsg, NULL) &&
          !pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          memcmp(win, &snap, sizeof(snap)) == 0 &&
          !pk_session_passive_auth_unmatched(&ctx, 40u, &rmsg, NULL) &&
          !pk_session_passive_auth_unmatched(&ctx, 42u, &rmsg, NULL),
          "P05.6", "unmatched 41-byte passive auth is dropped and does not enter the window");

    /* Exact 31- and 41-byte bodies: field 6 routing address, field 7 domain,
     * field 10 payload padded so the encoded size is the passive-auth length. */
    memset(frame31, 0, sizeof(frame31));
    frame31[0] = 0x32;
    frame31[1] = 18;
    frame31[2] = 0x12;
    frame31[3] = 16;
    memcpy(frame31 + 4, ROUTE2, 16);
    frame31[20] = 0x3a;
    frame31[21] = 2;
    frame31[22] = 0x08;
    frame31[23] = 2;
    frame31[24] = 0x52;
    frame31[25] = 5;
    for (i = 0; i < 5u; i++) {
        frame31[26u + i] = (uint8_t)(0xA0u + i);
    }
    memset(frame41, 0, sizeof(frame41));
    memcpy(frame41, frame31, 24);
    frame41[24] = 0x52;
    frame41[25] = 15;
    for (i = 0; i < 15u; i++) {
        frame41[26u + i] = (uint8_t)i;
    }
    check(pk_proto_parse_routable(frame31, sizeof(frame31), &raw) &&
          raw.has_to_routing_addr &&
          memcmp(raw.to_routing_addr, ROUTE2, 16) == 0 &&
          pk_session_passive_auth_unmatched(&empty, sizeof(frame31), &raw, NULL) &&
          !pk_session_passive_auth_unmatched(&empty, sizeof(frame31), &raw, ROUTE2) &&
          pk_proto_parse_routable(frame41, sizeof(frame41), &raw) &&
          pk_session_passive_auth_unmatched(&ctx, sizeof(frame41), &raw, ROUTE1) &&
          !pk_session_passive_auth_unmatched(&ctx, sizeof(frame41), &raw, ROUTE2),
          "P05.7", "exact 31- and 41-byte frames follow the routing-address rule");
    obs.rmsg = &raw;
    obs.frame_len = sizeof(frame41);
    obs.endpoint_routing = ROUTE1;
    check(!pk_session_accept_observation_rx(&ctx, &obs, plain, sizeof(plain), &plen, &ev) &&
          memcmp(win, &snap, sizeof(snap)) == 0,
          "P05.8", "exact 41-byte foreign frame does not move the window");
}

int main(void)
{
    printf("=== S2 Security Pre-Gate & N01-N08 Kernel Suite ===\n");
    test_pregate_1_bad_session_info_no_pollution();
    test_pregate_2_epoch_clock_and_pending_invalidation();
    test_pregate_3_rng_fail_closed();
    test_pregate_4_peer_identity_vs_continuity();
    test_n01_to_n08_kernel();
    test_req_id_required_no_domain_fallback();
    test_response_aad_counter_before_window();
    test_passive_auth_unmatched_not_in_window();

    printf("\nSUMMARY: %d PASS, %d FAIL (total %d)\n", g_pass, g_fail, g_pass + g_fail);
    return (g_fail == 0) ? 0 : 1;
}
