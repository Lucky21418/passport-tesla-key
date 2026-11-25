#include "pk_session.h"

#include <string.h>

static int session_index(pk_proto_domain_t domain)
{
    if (domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY) {
        return 0;
    }
    if (domain == PK_PROTO_DOMAIN_INFOTAINMENT) {
        return 1;
    }
    return -1;
}

void pk_session_init(pk_session_ctx_t *ctx)
{
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

void pk_session_set_conn_gen(pk_session_ctx_t *ctx, uint32_t conn_gen)
{
    if (!ctx) {
        return;
    }
    ctx->conn_gen = conn_gen;
    /* Invalidate pending attribution from prior connection generation */
    memset(ctx->handshake, 0, sizeof(ctx->handshake));
    memset(ctx->request, 0, sizeof(ctx->request));
    memset(ctx->signed_request, 0, sizeof(ctx->signed_request));
    pk_proto_rx_window_init(&ctx->rx_window[0]);
    pk_proto_rx_window_init(&ctx->rx_window[1]);
    memset(ctx->req_ids, 0, sizeof(ctx->req_ids));
    memset(ctx->req_id_next, 0, sizeof(ctx->req_id_next));
}

void pk_session_note_handshake_ex(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                                  pk_proto_domain_t domain,
                                  const uint8_t routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                                  const uint8_t uuid[PK_PROTO_UUID_LEN],
                                  pk_ms_t now_ms)
{
    int idx;
    pk_session_ref_t *rec;

    if (!ctx || !uuid || (idx = session_index(domain)) < 0) {
        return;
    }
    rec = &ctx->handshake[idx];
    memset(rec, 0, sizeof(*rec));
    rec->valid = true;
    rec->slot = slot;
    rec->conn_gen = conn_gen;
    if (routing_addr) {
        rec->has_routing_addr = true;
        memcpy(rec->routing_addr, routing_addr, PK_PROTO_ROUTING_ADDR_LEN);
    }
    memcpy(rec->uuid, uuid, PK_PROTO_UUID_LEN);
    rec->at_ms = now_ms;
}

void pk_session_note_handshake(pk_session_ctx_t *ctx, int slot,
                               pk_proto_domain_t domain,
                               const uint8_t uuid[PK_PROTO_UUID_LEN],
                               pk_ms_t now_ms)
{
    pk_session_note_handshake_ex(ctx, slot, ctx ? ctx->conn_gen : 0u, domain, NULL, uuid, now_ms);
}

void pk_session_note_request_ex(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                                uint32_t epoch_gen, pk_proto_domain_t domain,
                                const uint8_t routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                                const uint8_t uuid[PK_PROTO_UUID_LEN],
                                bool signed_request, pk_ms_t now_ms)
{
    int idx;
    pk_session_ref_t *rec;

    if (!ctx || !uuid || (idx = session_index(domain)) < 0) {
        return;
    }
    rec = &ctx->request[idx];
    memset(rec, 0, sizeof(*rec));
    rec->valid = true;
    rec->signed_request = signed_request;
    rec->challenge_used = false;
    rec->slot = slot;
    rec->conn_gen = conn_gen;
    rec->epoch_gen = epoch_gen;
    if (routing_addr) {
        rec->has_routing_addr = true;
        memcpy(rec->routing_addr, routing_addr, PK_PROTO_ROUTING_ADDR_LEN);
    }
    memcpy(rec->uuid, uuid, PK_PROTO_UUID_LEN);
    rec->at_ms = now_ms;
    if (signed_request) {
        ctx->signed_request[idx] = *rec;
    }
}

void pk_session_note_request(pk_session_ctx_t *ctx, int slot,
                             pk_proto_domain_t domain,
                             const uint8_t uuid[PK_PROTO_UUID_LEN],
                             bool signed_request, pk_ms_t now_ms)
{
    pk_session_note_request_ex(ctx, slot, ctx ? ctx->conn_gen : 0u, 0u, domain, NULL, uuid,
                               signed_request, now_ms);
}

bool pk_session_handshake_recent(const pk_session_ctx_t *ctx, int slot,
                                 pk_proto_domain_t domain, pk_ms_t now_ms,
                                 pk_ms_t window_ms)
{
    int idx;
    const pk_session_ref_t *rec;

    if (!ctx || (idx = session_index(domain)) < 0) {
        return false;
    }
    rec = &ctx->handshake[idx];
    return rec->valid && rec->slot == slot && rec->conn_gen == ctx->conn_gen &&
           !pk_time_elapsed(now_ms, rec->at_ms, window_ms);
}

static bool route_matches_ref(const pk_session_ref_t *ref, const pk_proto_routable_t *rmsg)
{
    if (!ref->has_routing_addr) {
        return true;
    }
    return rmsg->has_to_routing_addr &&
           memcmp(rmsg->to_routing_addr, ref->routing_addr, PK_PROTO_ROUTING_ADDR_LEN) == 0;
}

bool pk_session_apply(pk_session_ctx_t *ctx, const pk_session_apply_t *in,
                      pk_proto_session_t *sess)
{
    const uint8_t *challenge;
    pk_session_ref_t *hs;
    pk_session_ref_t *req;
    pk_proto_session_apply_t apply;
    bool used_signed = false;
    uint32_t expected_conn_gen;
    uint32_t prev_epoch_gen;
    int idx;

    if (!ctx || !in || !sess || !in->vin || !in->privkey || !in->rmsg) {
        return false;
    }
    if ((idx = session_index(in->domain)) < 0 ||
        in->domain != in->rmsg->from_domain) {
        return false;
    }
    if (!in->rmsg->has_session_info || !in->rmsg->has_session_info_tag) {
        return false;
    }

    expected_conn_gen = (in->conn_gen != 0u) ? in->conn_gen : ctx->conn_gen;
    if (expected_conn_gen != ctx->conn_gen) {
        return false;
    }

    hs = &ctx->handshake[idx];
    req = &ctx->signed_request[idx];
    if (in->rmsg->has_request_uuid) {
        if (hs->valid && hs->slot == in->slot && hs->conn_gen == expected_conn_gen &&
            route_matches_ref(hs, in->rmsg) &&
            memcmp(in->rmsg->request_uuid, hs->uuid, PK_PROTO_UUID_LEN) == 0) {
            if (pk_time_elapsed(in->now_ms, hs->at_ms, PK_SESSION_CHALLENGE_MS)) {
                return false;
            }
            challenge = hs->uuid;
        } else if (req->valid && !req->challenge_used && req->slot == in->slot &&
                   req->conn_gen == expected_conn_gen &&
                   route_matches_ref(req, in->rmsg) &&
                   memcmp(in->rmsg->request_uuid, req->uuid, PK_PROTO_UUID_LEN) == 0) {
            if (pk_time_elapsed(in->now_ms, req->at_ms, PK_SESSION_CHALLENGE_MS)) {
                return false;
            }
            challenge = req->uuid;
            used_signed = true;
        } else {
            return false;
        }
    } else {
        /* An unattributed SessionInfo is usable only by the live handshake. */
        if (!hs->valid || hs->slot != in->slot || hs->conn_gen != expected_conn_gen ||
            !route_matches_ref(hs, in->rmsg) ||
            pk_time_elapsed(in->now_ms, hs->at_ms, PK_SESSION_CHALLENGE_MS)) {
            return false;
        }
        challenge = hs->uuid;
    }

    prev_epoch_gen = sess->epoch_gen;
    apply = (pk_proto_session_apply_t){
        .info = {
            .vin              = in->vin,
            .challenge        = challenge,
            .challenge_len    = PK_PROTO_UUID_LEN,
            .encoded_info     = in->rmsg->session_info_bytes,
            .encoded_info_len = in->rmsg->session_info_len,
        },
        .privkey = in->privkey,
        .tag     = in->rmsg->session_info_tag,
        .tag_len = in->rmsg->session_info_tag_len,
        .now_ms  = in->session_now_ms,
    };
    if (!pk_proto_session_apply_info(sess, &apply)) {
        /* Failed SessionInfo MUST NOT consume handshake or pollute pending state */
        return false;
    }

    hs->valid = false;
    if (used_signed) {
        req->challenge_used = true;
    }
    /* Security pre-gate #2: when epoch changes on an existing session, old
     * pending request attribution on this domain is invalidated so late
     * responses from the prior epoch cannot complete a new transaction. */
    if (sess->epoch_gen != prev_epoch_gen) {
        pk_proto_rx_window_init(&ctx->rx_window[idx]);
        memset(ctx->req_ids[idx], 0, sizeof(ctx->req_ids[idx]));
        ctx->req_id_next[idx] = 0u;
    }
    if (prev_epoch_gen != 0u && sess->epoch_gen != prev_epoch_gen) {
        ctx->request[idx].valid = false;
        if (!used_signed) {
            ctx->signed_request[idx].valid = false;
        }
    }
    return true;
}

bool pk_session_accept_rx(const pk_session_ctx_t *ctx,
                          const pk_session_rx_in_t *in,
                          uint8_t *plain_out, size_t plain_cap,
                          size_t *plain_len_out,
                          pk_evidence_t *evidence_out)
{
    int idx;
    const pk_session_ref_t *rec;

    if (evidence_out) {
        *evidence_out = PK_EVIDENCE_UNKNOWN;
    }
    if (plain_len_out) {
        *plain_len_out = 0u;
    }
    if (!ctx || !in || !in->rmsg || !plain_out || !plain_len_out || !evidence_out) {
        return false;
    }
    if ((idx = session_index(in->domain)) < 0 ||
        !in->rmsg->has_from_domain || in->rmsg->from_domain != in->domain ||
        !in->rmsg->has_payload_bytes || in->rmsg->payload_len == 0u ||
        in->rmsg->payload_len > plain_cap) {
        return false;
    }

    /* 1. Attribution: active txn, slot, conn_gen, epoch_gen, deadline */
    if (!in->txn_active || in->txn_slot != in->slot ||
        in->conn_gen != ctx->conn_gen || in->txn_conn_gen != in->conn_gen ||
        pk_time_due(in->now_ms, in->txn_deadline)) {
        return false;
    }
    if (in->sess && in->txn_epoch_gen != 0u && in->sess->epoch_gen != in->txn_epoch_gen) {
        return false;
    }

    /* Mandatory 16-byte to_routing_addr match against live txn */
    if (!in->txn_routing_addr || !in->rmsg->has_to_routing_addr ||
        memcmp(in->rmsg->to_routing_addr, in->txn_routing_addr, PK_PROTO_ROUTING_ADDR_LEN) != 0) {
        return false;
    }

    /* Optional request_uuid: if present, MUST match txn_uuid */
    if (in->rmsg->has_request_uuid) {
        if (!in->txn_uuid ||
            memcmp(in->rmsg->request_uuid, in->txn_uuid, PK_PROTO_UUID_LEN) != 0) {
            return false;
        }
    }

    /* Cross-check session request attribution record */
    rec = &ctx->request[idx];
    if (!rec->valid || rec->slot != in->slot || rec->conn_gen != in->conn_gen) {
        return false;
    }
    if (in->rmsg->has_request_uuid &&
        memcmp(in->rmsg->request_uuid, rec->uuid, PK_PROTO_UUID_LEN) != 0) {
        return false;
    }

    /* 2. Cryptographic authentication vs Legacy plaintext evidence separation */
    if (in->rmsg->has_sig_data || in->rmsg->has_aes_gcm_resp) {
        pk_proto_decrypt_t dec = { in->sess, in->vin, in->txn_req_id };
        size_t dec_len = 0u;

        /* Auth-before-commit (D02): decrypt and verify GCM tag BEFORE touching rx_window! */
        if (!in->rx_window ||
            !pk_proto_decrypt_routable_payload(in->rmsg, &dec, plain_out, plain_cap, &dec_len)) {
            pk_proto_zeroize(plain_out, plain_cap);
            return false;
        }
        if (!pk_proto_rx_window_accept(in->rx_window, in->rmsg->resp_counter)) {
            pk_proto_zeroize(plain_out, plain_cap);
            return false;
        }
        *plain_len_out = dec_len;
        *evidence_out = PK_EVIDENCE_CRYPTO_AUTHENTICATED;
        return true;
    }

    /* Plaintext legacy reply: attributed only, NEVER CRYPTO_AUTHENTICATED */
    memcpy(plain_out, in->rmsg->payload_bytes, in->rmsg->payload_len);
    *plain_len_out = in->rmsg->payload_len;
    *evidence_out = PK_EVIDENCE_LEGACY_ATTRIBUTED;
    return true;
}

pk_proto_rx_window_t *pk_session_rx_window(pk_session_ctx_t *ctx, pk_proto_domain_t domain)
{
    int idx;
    if (!ctx || (idx = session_index(domain)) < 0) {
        return NULL;
    }
    return &ctx->rx_window[idx];
}

void pk_session_note_req_id(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                            pk_proto_domain_t domain, const uint8_t req_id[17])
{
    int idx;
    uint8_t pos;

    if (!ctx || !req_id || conn_gen != ctx->conn_gen || (idx = session_index(domain)) < 0) {
        return;
    }
    pos = (uint8_t)(ctx->req_id_next[idx] % PK_SESSION_REQ_ID_CAP);
    ctx->req_ids[idx][pos].valid = true;
    ctx->req_ids[idx][pos].slot = slot;
    memcpy(ctx->req_ids[idx][pos].req_id, req_id, 17u);
    ctx->req_id_next[idx] = (uint8_t)((pos + 1u) % PK_SESSION_REQ_ID_CAP);
}

bool pk_session_accept_observation_rx(pk_session_ctx_t *ctx,
                                      const pk_session_obs_in_t *in,
                                      uint8_t *plain_out, size_t plain_cap,
                                      size_t *plain_len_out,
                                      pk_evidence_t *evidence_out)
{
    int idx;
    pk_proto_decrypt_t dec;
    size_t dec_len = 0u;
    bool decrypted = false;

    if (evidence_out) {
        *evidence_out = PK_EVIDENCE_UNKNOWN;
    }
    if (plain_len_out) {
        *plain_len_out = 0u;
    }
    if (!ctx || !in || !in->rmsg || !in->sess || !in->sess->valid ||
        !plain_out || !plain_len_out || !evidence_out) {
        return false;
    }
    if ((idx = session_index(in->domain)) < 0 ||
        in->conn_gen != ctx->conn_gen ||
        !in->rmsg->has_from_domain || in->rmsg->from_domain != in->domain ||
        !in->rmsg->has_payload_bytes || in->rmsg->payload_len == 0u ||
        in->rmsg->payload_len > plain_cap) {
        return false;
    }
    /* Passive state observations MUST be cryptographically authenticated;
     * unauthenticated plaintext is never accepted without an attributed txn. */
    if (!in->rmsg->has_sig_data && !in->rmsg->has_aes_gcm_resp) {
        return false;
    }
    dec.sess = in->sess;
    dec.vin = in->vin;
    if (in->req_id) {
        dec.req_id = in->req_id;
        decrypted = pk_proto_decrypt_routable_payload(in->rmsg, &dec, plain_out, plain_cap, &dec_len);
    } else {
        uint8_t step;
        for (step = 0u; step < PK_SESSION_REQ_ID_CAP; ++step) {
            uint8_t pos = (uint8_t)((ctx->req_id_next[idx] + PK_SESSION_REQ_ID_CAP - 1u - step) %
                                    PK_SESSION_REQ_ID_CAP);
            const pk_session_req_id_t *rid = &ctx->req_ids[idx][pos];
            if (!rid->valid || rid->slot != in->slot) {
                continue;
            }
            dec.req_id = rid->req_id;
            if (pk_proto_decrypt_routable_payload(in->rmsg, &dec, plain_out, plain_cap, &dec_len)) {
                decrypted = true;
                break;
            }
        }
    }
    if (!decrypted) {
        pk_proto_zeroize(plain_out, plain_cap);
        return false;
    }
    if (!pk_proto_rx_window_accept(&ctx->rx_window[idx], in->rmsg->resp_counter)) {
        pk_proto_zeroize(plain_out, plain_cap);
        return false;
    }
    *plain_len_out = dec_len;
    *evidence_out = PK_EVIDENCE_CRYPTO_AUTHENTICATED;
    return true;
}

bool pk_session_accept_pre_session_pair(bool pair_step3_active,
                                        bool vcsec_session_valid,
                                        const uint8_t *wire, size_t wire_len,
                                        pk_proto_vcsec_resp_t *vresp_out,
                                        pk_evidence_t *evidence_out)
{
    pk_proto_routable_t rmsg;
    const uint8_t *vcsec_buf = wire;
    size_t vcsec_len = wire_len;

    if (evidence_out) {
        *evidence_out = PK_EVIDENCE_UNKNOWN;
    }
    if (!pair_step3_active || vcsec_session_valid || !wire || wire_len == 0u ||
        wire_len > PK_PROTO_MAX_FRAME_LEN || !vresp_out) {
        return false;
    }

    /* Pre-session AddKey replies from VCSEC may arrive either wrapped in an
     * unsigned RoutableMessage from DOMAIN_VEHICLE_SECURITY or as a bare
     * FromVCSECMessage carrying WhitelistOperationStatus. Restrict strictly
     * to CommandStatus + WhitelistOperationStatus during pre-session Step 3. */
    if (pk_proto_parse_routable(wire, wire_len, &rmsg) &&
        rmsg.has_from_domain &&
        rmsg.from_domain == PK_PROTO_DOMAIN_VEHICLE_SECURITY &&
        rmsg.has_payload_bytes && !rmsg.has_sig_data) {
        vcsec_buf = rmsg.payload_bytes;
        vcsec_len = rmsg.payload_len;
    }

    if (!pk_proto_parse_vcsec_response(vcsec_buf, vcsec_len, vresp_out)) {
        return false;
    }
    if (!vresp_out->has_command_status || !vresp_out->has_whitelist_status) {
        memset(vresp_out, 0, sizeof(*vresp_out));
        return false;
    }
    if (evidence_out) {
        *evidence_out = PK_EVIDENCE_LEGACY_ATTRIBUTED;
    }
    return true;
}

bool pk_session_fault_gate_ex(const pk_session_ctx_t *ctx, int slot,
                              uint32_t conn_gen, pk_proto_domain_t domain,
                              const pk_proto_routable_t *rmsg,
                              pk_ms_t txn_now_ms, pk_ms_t session_now_ms,
                              bool txn_active, int txn_slot, uint32_t txn_conn_gen,
                              pk_ms_t txn_deadline,
                              const uint8_t txn_uuid[PK_PROTO_UUID_LEN],
                              const uint8_t txn_routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                              pk_action_t txn_action, int fault)
{
    uint32_t effective_gen;
    bool live_txn;
    (void)txn_action;
    (void)fault;

    if (!ctx || !rmsg || session_index(domain) < 0 ||
        rmsg->from_domain != domain) {
        return false;
    }
    effective_gen = (conn_gen != 0u) ? conn_gen : ctx->conn_gen;
    if (effective_gen != ctx->conn_gen) {
        return false;
    }
    /* S2 §2C: Every live-txn fault MUST match active txn, slot, conn_gen,
     * domain, deadline, mandatory 16-byte to_routing_addr, and request_uuid
     * (when present). A fresh SessionInfo in the same packet NEVER bypasses
     * fault attribution (S2 §1). */
    live_txn = txn_active &&
        txn_slot == slot &&
        (txn_conn_gen == 0u || txn_conn_gen == effective_gen) &&
        !pk_time_due(txn_now_ms, txn_deadline) &&
        txn_routing_addr != NULL &&
        rmsg->has_to_routing_addr &&
        memcmp(rmsg->to_routing_addr, txn_routing_addr, PK_PROTO_ROUTING_ADDR_LEN) == 0 &&
        (!rmsg->has_request_uuid ||
         (txn_uuid &&
          memcmp(rmsg->request_uuid, txn_uuid, PK_PROTO_UUID_LEN) == 0));
    return live_txn ||
           pk_session_fault_matches(ctx, slot, domain, rmsg, session_now_ms);
}

bool pk_session_fault_gate(const pk_session_ctx_t *ctx, int slot,
                           pk_proto_domain_t domain,
                           const pk_proto_routable_t *rmsg,
                           pk_ms_t txn_now_ms, pk_ms_t session_now_ms,
                           bool txn_active, int txn_slot, pk_ms_t txn_deadline,
                           const uint8_t txn_uuid[PK_PROTO_UUID_LEN],
                           const uint8_t txn_routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                           pk_action_t txn_action, int fault,
                           bool fresh_session_applied)
{
    (void)fresh_session_applied; /* SessionInfo HMAC never endorses an outer fault */
    return pk_session_fault_gate_ex(ctx, slot, 0u, domain, rmsg,
                                    txn_now_ms, session_now_ms,
                                    txn_active, txn_slot, 0u, txn_deadline,
                                    txn_uuid, txn_routing_addr,
                                    txn_action, fault);
}

bool pk_session_fault_matches(const pk_session_ctx_t *ctx, int slot,
                              pk_proto_domain_t domain,
                              const pk_proto_routable_t *rmsg, pk_ms_t now_ms)
{
    int idx;
    const pk_session_ref_t *hs;
    const pk_session_ref_t *req;

    if (!ctx || !rmsg || (idx = session_index(domain)) < 0 ||
        domain != rmsg->from_domain) {
        return false;
    }
    hs = &ctx->handshake[idx];
    req = &ctx->request[idx];
    if (rmsg->has_request_uuid) {
        if (hs->valid && hs->slot == slot && hs->conn_gen == ctx->conn_gen &&
            route_matches_ref(hs, rmsg) &&
            !pk_time_elapsed(now_ms, hs->at_ms, PK_SESSION_CHALLENGE_MS) &&
            memcmp(rmsg->request_uuid, hs->uuid, PK_PROTO_UUID_LEN) == 0) {
            return true;
        }
        if (req->valid && req->slot == slot && req->conn_gen == ctx->conn_gen &&
            route_matches_ref(req, rmsg) &&
            !pk_time_elapsed(now_ms, req->at_ms, PK_SESSION_CHALLENGE_MS) &&
            memcmp(rmsg->request_uuid, req->uuid, PK_PROTO_UUID_LEN) == 0) {
            return true;
        }
        return false;
    }
    return hs->valid && hs->slot == slot && hs->conn_gen == ctx->conn_gen &&
           route_matches_ref(hs, rmsg) &&
           !pk_time_elapsed(now_ms, hs->at_ms, PK_SESSION_CHALLENGE_MS);
}

bool pk_session_correlate(const pk_session_ctx_t *ctx, int slot,
                          pk_proto_domain_t domain,
                          const pk_proto_routable_t *rmsg)
{
    int idx;
    const pk_session_ref_t *rec;

    if (!ctx || !rmsg || (idx = session_index(domain)) < 0 ||
        domain != rmsg->from_domain) {
        return false;
    }
    rec = &ctx->request[idx];
    if (!rec->valid || rec->slot != slot || rec->conn_gen != ctx->conn_gen) {
        return false;
    }
    if (!route_matches_ref(rec, rmsg)) {
        return false;
    }
    if (rmsg->has_request_uuid &&
        memcmp(rmsg->request_uuid, rec->uuid, PK_PROTO_UUID_LEN) != 0) {
        return false;
    }
    return true;
}

static bool fault_drops_session(int fault)
{
    return pk_proto_fault_needs_resync(fault) ||
           pk_proto_fault_is_key_error(fault) ||
           fault == PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES ||
           fault == PK_PROTO_FAULT_REQUIRES_RESPONSE_ENCRYPTION;
}

bool pk_session_clear_on_fault(pk_session_ctx_t *ctx, pk_proto_domain_t domain,
                               pk_proto_session_t *sess, int fault,
                               bool fresh_session_applied)
{
    int idx;

    if (!ctx || (idx = session_index(domain)) < 0 || !fault_drops_session(fault)) {
        return false;
    }
    ctx->handshake[idx].valid = false;
    ctx->request[idx].valid = false;
    ctx->signed_request[idx].valid = false;
    if (fresh_session_applied || !sess) {
        return false;
    }
    pk_proto_session_clear(sess);
    return true;
}

void pk_session_drop(pk_session_ctx_t *ctx, pk_proto_domain_t domain,
                     pk_proto_session_t *sess)
{
    int idx;

    if (!ctx || (idx = session_index(domain)) < 0) {
        return;
    }
    ctx->handshake[idx].valid = false;
    ctx->request[idx].valid = false;
    ctx->signed_request[idx].valid = false;
    if (sess) {
        pk_proto_session_clear(sess);
    }
}

void pk_session_on_disconnect(pk_session_ctx_t *ctx, int slot)
{
    unsigned i;

    if (!ctx) {
        return;
    }
    for (i = 0; i < PK_SESSION_DOMAIN_COUNT; i++) {
        if (slot == PK_VEHICLE_NONE || ctx->handshake[i].slot == slot) {
            ctx->handshake[i].valid = false;
        }
        if (slot == PK_VEHICLE_NONE || ctx->request[i].slot == slot) {
            ctx->request[i].valid = false;
        }
        if (slot == PK_VEHICLE_NONE || ctx->signed_request[i].slot == slot) {
            ctx->signed_request[i].valid = false;
        }
    }
}
