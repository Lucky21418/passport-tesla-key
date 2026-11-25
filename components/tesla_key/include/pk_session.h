#ifndef PK_SESSION_H
#define PK_SESSION_H

#include "pk_protocol.h"

/*
 * Single authoritative session, attribution, authentication, and anti-replay
 * acceptance kernel shared by every transport port (Provider A).
 *
 * The BLE port moves bytes and tracks physical GATT handles; this kernel owns:
 *   - dual-domain isolation (VCSEC / Infotainment),
 *   - connection-generation (conn_gen) and epoch-generation invalidation,
 *   - SessionInfo challenge/HMAC verification and in-ctx peer continuity,
 *   - single RX acceptance path (attribution -> AEAD auth -> sliding-window commit),
 *   - explicit evidence classification (CRYPTO_AUTHENTICATED vs LEGACY_ATTRIBUTED),
 *   - explicit isolated pre-session pairing AddKey Step 3 entry.
 */

#define PK_SESSION_DOMAIN_COUNT  2
#define PK_SESSION_CHALLENGE_MS  4000u

typedef struct {
    bool     valid;
    bool     signed_request; /* only signed requests may authenticate SessionInfo */
    bool     challenge_used; /* signed request already authenticated one SessionInfo */
    int      slot;
    uint32_t conn_gen;
    uint32_t epoch_gen;
    bool     has_routing_addr;
    uint8_t  routing_addr[PK_PROTO_ROUTING_ADDR_LEN];
    uint8_t  uuid[PK_PROTO_UUID_LEN];
    pk_ms_t  at_ms;
} pk_session_ref_t;

#define PK_SESSION_REQ_ID_CAP    4

typedef struct {
    bool    valid;
    int     slot;
    uint8_t req_id[17];
} pk_session_req_id_t;

typedef struct {
    uint32_t             conn_gen;
    pk_session_ref_t     handshake[PK_SESSION_DOMAIN_COUNT];
    pk_session_ref_t     request[PK_SESSION_DOMAIN_COUNT];        /* last request, any */
    pk_session_ref_t     signed_request[PK_SESSION_DOMAIN_COUNT]; /* last signed request */
    pk_proto_rx_window_t rx_window[PK_SESSION_DOMAIN_COUNT];      /* session anti-replay (conn_gen + epoch_gen) */
    pk_session_req_id_t  req_ids[PK_SESSION_DOMAIN_COUNT][PK_SESSION_REQ_ID_CAP];
    uint8_t              req_id_next[PK_SESSION_DOMAIN_COUNT];
} pk_session_ctx_t;

typedef struct {
    int                        slot;
    uint32_t                   conn_gen; /* 0 = legacy caller using ctx->conn_gen */
    pk_proto_domain_t          domain;
    const char                *vin;
    const uint8_t             *privkey; /* [PK_PROTO_PRIVKEY_LEN] */
    const pk_proto_routable_t *rmsg;
    pk_ms_t                    now_ms;         /* challenge-age watermark */
    pk_ms_t                    session_now_ms; /* app watermark for sync_local_ms */
} pk_session_apply_t;

typedef struct {
    int                        slot;
    uint32_t                   conn_gen;
    pk_proto_domain_t          domain;
    const char                *vin;
    const pk_proto_session_t  *sess;
    const pk_proto_routable_t *rmsg;
    pk_ms_t                    now_ms;
    bool                       txn_active;
    int                        txn_slot;
    uint32_t                   txn_conn_gen;
    uint32_t                   txn_epoch_gen;
    pk_ms_t                    txn_deadline;
    const uint8_t             *txn_routing_addr; /* [PK_PROTO_ROUTING_ADDR_LEN] */
    const uint8_t             *txn_uuid;         /* [PK_PROTO_UUID_LEN] */
    const uint8_t             *txn_req_id;       /* [17] */
    pk_proto_rx_window_t      *rx_window;
} pk_session_rx_in_t;

void pk_session_init(pk_session_ctx_t *ctx);
void pk_session_set_conn_gen(pk_session_ctx_t *ctx, uint32_t conn_gen);

void pk_session_note_handshake_ex(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                                  pk_proto_domain_t domain,
                                  const uint8_t routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                                  const uint8_t uuid[PK_PROTO_UUID_LEN],
                                  pk_ms_t now_ms);

void pk_session_note_handshake(pk_session_ctx_t *ctx, int slot,
                               pk_proto_domain_t domain,
                               const uint8_t uuid[PK_PROTO_UUID_LEN],
                               pk_ms_t now_ms);

void pk_session_note_request_ex(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                                uint32_t epoch_gen, pk_proto_domain_t domain,
                                const uint8_t routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                                const uint8_t uuid[PK_PROTO_UUID_LEN],
                                bool signed_request, pk_ms_t now_ms);

void pk_session_note_request(pk_session_ctx_t *ctx, int slot,
                             pk_proto_domain_t domain,
                             const uint8_t uuid[PK_PROTO_UUID_LEN],
                             bool signed_request, pk_ms_t now_ms);

bool pk_session_handshake_recent(const pk_session_ctx_t *ctx, int slot,
                                 pk_proto_domain_t domain, pk_ms_t now_ms,
                                 pk_ms_t window_ms);

/* Apply a vehicle SessionInfo after checking slot, conn_gen, routing_addr
 * (when recorded), UUID + 4 s challenge window, and 32-byte HMAC-SHA256.
 * Never pollutes existing session/pending on failure.
 * When epoch changes on an existing session, invalidates old pending request
 * attribution on that domain so late replies cannot complete a new txn. */
bool pk_session_apply(pk_session_ctx_t *ctx, const pk_session_apply_t *in,
                      pk_proto_session_t *sess);

/* Single authoritative RX acceptance entry (D02 + D03):
 * Enforces slot, conn_gen, epoch_gen, domain, deadline, mandatory 16-byte
 * to_routing_addr, optional request_uuid, auth-before-commit into rx_window
 * for AEAD responses, and strict evidence separation (never upgrades
 * LEGACY_ATTRIBUTED to CRYPTO_AUTHENTICATED). */
bool pk_session_accept_rx(const pk_session_ctx_t *ctx,
                          const pk_session_rx_in_t *in,
                          uint8_t *plain_out, size_t plain_cap,
                          size_t *plain_len_out,
                          pk_evidence_t *evidence_out);

/* Return the domain session anti-replay window (reset on conn_gen / epoch_gen). */
pk_proto_rx_window_t *pk_session_rx_window(pk_session_ctx_t *ctx, pk_proto_domain_t domain);

/* Record a signed request's 17-byte req_id on the domain session so passive
 * AEAD observations can verify AAD even after the short-lived wire txn ends. */
void pk_session_note_req_id(pk_session_ctx_t *ctx, int slot, uint32_t conn_gen,
                            pk_proto_domain_t domain, const uint8_t req_id[17]);

typedef struct {
    int                        slot;
    uint32_t                   conn_gen;
    pk_proto_domain_t          domain;
    const char                *vin;
    const pk_proto_session_t  *sess;
    const pk_proto_routable_t *rmsg;
    const uint8_t             *req_id; /* optional explicit [17]; NULL uses domain session req_id ring */
} pk_session_obs_in_t;

/* Passive authenticated state-observation RX track:
 * Accepts AEAD-authenticated vehicle state snapshots on the active domain
 * session even when no command txn is active or after a txn has ended.
 * Enforces conn_gen, valid session, domain match, AEAD auth-before-commit into
 * the session rx_window; never accepts unauthenticated plaintext. */
bool pk_session_accept_observation_rx(pk_session_ctx_t *ctx,
                                      const pk_session_obs_in_t *in,
                                      uint8_t *plain_out, size_t plain_cap,
                                      size_t *plain_len_out,
                                      pk_evidence_t *evidence_out);

/* Explicit isolated pre-session pairing acceptance entry (D03):
 * Accepts a pre-session VCSEC WhitelistOperationStatus reply ONLY while
 * pairing step 3 is active and no VCSEC session exists yet. Never used for
 * normal operational traffic; always classifies as PK_EVIDENCE_LEGACY_ATTRIBUTED. */
bool pk_session_accept_pre_session_pair(bool pair_step3_active,
                                        bool vcsec_session_valid,
                                        const uint8_t *wire, size_t wire_len,
                                        pk_proto_vcsec_resp_t *vresp_out,
                                        pk_evidence_t *evidence_out);

bool pk_session_fault_gate_ex(const pk_session_ctx_t *ctx, int slot,
                              uint32_t conn_gen, pk_proto_domain_t domain,
                              const pk_proto_routable_t *rmsg,
                              pk_ms_t txn_now_ms, pk_ms_t session_now_ms,
                              bool txn_active, int txn_slot, uint32_t txn_conn_gen,
                              pk_ms_t txn_deadline,
                              const uint8_t txn_uuid[PK_PROTO_UUID_LEN],
                              const uint8_t txn_routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                              pk_action_t txn_action, int fault);

bool pk_session_fault_gate(const pk_session_ctx_t *ctx, int slot,
                           pk_proto_domain_t domain,
                           const pk_proto_routable_t *rmsg,
                           pk_ms_t txn_now_ms, pk_ms_t session_now_ms,
                           bool txn_active, int txn_slot, pk_ms_t txn_deadline,
                           const uint8_t txn_uuid[PK_PROTO_UUID_LEN],
                           const uint8_t txn_routing_addr[PK_PROTO_ROUTING_ADDR_LEN],
                           pk_action_t txn_action, int fault,
                           bool fresh_session_applied);

bool pk_session_fault_matches(const pk_session_ctx_t *ctx, int slot,
                              pk_proto_domain_t domain,
                              const pk_proto_routable_t *rmsg, pk_ms_t now_ms);

bool pk_session_correlate(const pk_session_ctx_t *ctx, int slot,
                          pk_proto_domain_t domain,
                          const pk_proto_routable_t *rmsg);

bool pk_session_clear_on_fault(pk_session_ctx_t *ctx, pk_proto_domain_t domain,
                               pk_proto_session_t *sess, int fault,
                               bool fresh_session_applied);

void pk_session_drop(pk_session_ctx_t *ctx, pk_proto_domain_t domain,
                     pk_proto_session_t *sess);

void pk_session_on_disconnect(pk_session_ctx_t *ctx, int slot);

#endif /* PK_SESSION_H */
