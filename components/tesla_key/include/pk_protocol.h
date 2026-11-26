#ifndef PK_PROTOCOL_H
#define PK_PROTOCOL_H

#include "pk_app.h"
#include "pk_types.h"
#include "pk_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native C implementation of the Tesla BLE Vehicle Command Protocol
 * (Provider A: official teslamotors/vehicle-command @ 0a582da4 schema +
 * nanopb 0.4.9.1 generated codecs + bounded C11 thin session + IDF Mbed TLS):
 *   - GATT Service: 00000211-b2d1-43f0-9b88-960cebf8b91e
 *   - TX Char:      00000212-b2d1-43f0-9b88-960cebf8b91e
 *   - RX Char:      00000213-b2d1-43f0-9b88-960cebf8b91e
 *   - Local Name:   "S" + lower_hex(SHA1(VIN)[0..7]) + "C" (18 chars)
 *   - Wire Framing: 2-byte big-endian length prefix + protobuf payload
 *   - Session Auth: ECDH P-256 -> K = SHA1(Sx)[0..15] -> AES-128-GCM Personalized
 *
 * Strictly enforces one-shot semantics: no automatic write replay, no cloud wakeup.
 */

#define PK_PROTO_BLE_NAME_LEN        18
#define PK_PROTO_BLE_NAME_BUF        19
#define PK_PROTO_PUBKEY_LEN          65
#define PK_PROTO_PRIVKEY_LEN         32
#define PK_PROTO_SESSION_KEY_LEN     16
#define PK_PROTO_EPOCH_LEN           16
#define PK_PROTO_NONCE_LEN           12
#define PK_PROTO_TAG_LEN             16
#define PK_PROTO_HMAC_LEN            32
#define PK_PROTO_ROUTING_ADDR_LEN    16
#define PK_PROTO_UUID_LEN            16
#define PK_PROTO_MAX_FRAME_LEN       1024
#define PK_PROTO_RX_TIMEOUT_MS       1000u
/* Issue #390 passive phone-key authentication messages. Length is the
 * reassembled payload (the 2-byte big-endian prefix value). */
#define PK_PROTO_PASSIVE_AUTH_LEN_A  31u
#define PK_PROTO_PASSIVE_AUTH_LEN_B  41u
/* UniversalMessage.Flags: request encrypted responses on every signed BLE command. */
#define PK_PROTO_FLAG_ENCRYPT_RESPONSE 0x00000002u

/* Production entropy-source readiness bits (ESP-IDF 5.5.3 esp_random.h requires
 * BLE or Wi-Fi RF subsystem active for true hardware entropy). */
#define PK_ENTROPY_SRC_BLE           (1u << 0)
#define PK_ENTROPY_SRC_WIFI          (1u << 1)

/* Evidence tier for accepted incoming replies (D03):
 * LEGACY_ATTRIBUTED MUST NEVER be promoted to CRYPTO_AUTHENTICATED or grant qualification. */
typedef enum {
    PK_EVIDENCE_UNKNOWN = 0,
    PK_EVIDENCE_LEGACY_ATTRIBUTED = 1,
    PK_EVIDENCE_CRYPTO_AUTHENTICATED = 2
} pk_evidence_t;

/* UniversalMessage.Domain */
typedef enum {
    PK_PROTO_DOMAIN_BROADCAST        = 0,
    PK_PROTO_DOMAIN_VEHICLE_SECURITY = 2,
    PK_PROTO_DOMAIN_INFOTAINMENT     = 3
} pk_proto_domain_t;

/* VCSEC.KeyFormFactor */
typedef enum {
    PK_PROTO_KEY_FORM_UNKNOWN        = 0,
    PK_PROTO_KEY_FORM_NFC_CARD       = 1,
    PK_PROTO_KEY_FORM_IOS_DEVICE     = 6,
    PK_PROTO_KEY_FORM_ANDROID_DEVICE = 7,
    PK_PROTO_KEY_FORM_CLOUD_KEY      = 9
} pk_proto_key_form_t;

/* Keys.Role. Values match keys.proto Role
 * (teslamotors/vehicle-command @ 0a582da). Pairing uses
 * pk_proto_pair_key_role(), which is Driver. */
typedef enum {
    PK_PROTO_ROLE_NONE               = 0,
    PK_PROTO_ROLE_SERVICE            = 1,
    PK_PROTO_ROLE_OWNER              = 2,
    PK_PROTO_ROLE_DRIVER             = 3,
    PK_PROTO_ROLE_FM                 = 4,
    PK_PROTO_ROLE_VEHICLE_MONITOR    = 5,
    PK_PROTO_ROLE_CHARGING_MANAGER   = 6,
    PK_PROTO_ROLE_GUEST              = 8
} pk_proto_role_t;

/* Least official role that can lock, unlock, and move both trunks. */
pk_proto_role_t pk_proto_pair_key_role(void);

/* VCSEC.RKEAction_E */
typedef enum {
    PK_PROTO_RKE_UNLOCK              = 0,
    PK_PROTO_RKE_LOCK                = 1,
    PK_PROTO_RKE_AUTO_SECURE         = 29,
    PK_PROTO_RKE_WAKE_VEHICLE        = 30
} pk_proto_rke_action_t;

/* VCSEC.ClosureMoveType_E */
typedef enum {
    PK_PROTO_CLOSURE_MOVE_NONE       = 0,
    PK_PROTO_CLOSURE_MOVE_MOVE       = 1,
    PK_PROTO_CLOSURE_MOVE_STOP       = 2,
    PK_PROTO_CLOSURE_MOVE_OPEN       = 3,
    PK_PROTO_CLOSURE_MOVE_CLOSE      = 4
} pk_proto_closure_move_t;

/* VCSEC.InformationRequestType */
typedef enum {
    PK_PROTO_INFO_GET_STATUS               = 0,
    PK_PROTO_INFO_GET_WHITELIST_INFO       = 5,
    PK_PROTO_INFO_GET_WHITELIST_ENTRY_INFO = 6
} pk_proto_info_req_t;

/* Per-domain RAM session state.
 * NOTE: has_peer_pubkey enforces in-ctx peer identity continuity ONLY after
 * first SessionInfo establishment in this RAM context; it is NOT persistent
 * cross-reboot NVS pinning (S2 does not alter NVS identity format). */
typedef struct {
    bool valid;
    bool has_peer_pubkey;                          /* in-ctx first peer identity established */
    uint32_t epoch_gen;                            /* bumped on every epoch transition */
    uint32_t counter;
    uint8_t epoch[PK_PROTO_EPOCH_LEN];
    uint8_t vehicle_pubkey[PK_PROTO_PUBKEY_LEN];
    uint8_t session_key[PK_PROTO_SESSION_KEY_LEN]; /* SHA1(ECDH_x)[0..15] */
    uint32_t clock_time;                           /* vehicle seconds at sync */
    pk_ms_t sync_local_ms;                         /* local monotonic ms at sync */
    uint32_t handle;
    uint32_t flags;
} pk_proto_session_t;

/* Official vehicle-command windowSize (internal/authentication/crypto.go).
 * The product anti-replay window is aligned to this official width.
 * history is a uint64 bitmap because that is the verifier storage type
 * (the official comment requires the width to be at most 64). Freshness
 * uses PK_PROTO_RX_WINDOW_SIZE, which is the official value 32. */
#define PK_PROTO_RX_WINDOW_SIZE 32u

/* Per-request response anti-replay window, aligned to official width 32. */
typedef struct {
    bool     used;
    uint32_t counter;
    uint64_t history; /* bit (age-1) for age in [1..32] strictly below counter */
} pk_proto_rx_window_t;

void pk_proto_rx_window_init(pk_proto_rx_window_t *w);
bool pk_proto_rx_window_accept(pk_proto_rx_window_t *w, uint32_t counter);

/* UniversalMessage.MessageFault_E values we act on. */
typedef enum {
    PK_PROTO_FAULT_NONE = 0,
    PK_PROTO_FAULT_BUSY = 1,
    PK_PROTO_FAULT_TIMEOUT = 2,
    PK_PROTO_FAULT_UNKNOWN_KEY = 3,
    PK_PROTO_FAULT_INACTIVE_KEY = 4,
    PK_PROTO_FAULT_INVALID_SIGNATURE = 5,
    PK_PROTO_FAULT_INVALID_COUNTER = 6,
    PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES = 7,
    PK_PROTO_FAULT_INTERNAL = 11,
    PK_PROTO_FAULT_WRONG_VIN = 12,
    PK_PROTO_FAULT_INCORRECT_EPOCH = 15,
    PK_PROTO_FAULT_TIME_EXPIRED = 17,
    PK_PROTO_FAULT_TTL_TOO_LONG = 20,
    PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED = 25,
    PK_PROTO_FAULT_REPEATED_COUNTER = 26,
    PK_PROTO_FAULT_INVALID_KEY_HANDLE = 27,
    PK_PROTO_FAULT_REQUIRES_RESPONSE_ENCRYPTION = 28
} pk_proto_fault_t;

void pk_proto_session_clear(pk_proto_session_t *sess);
bool pk_proto_fault_needs_resync(int fault);
bool pk_proto_fault_is_key_error(int fault);

typedef struct {
    bool has_keypair;
    uint8_t privkey[PK_PROTO_PRIVKEY_LEN];
    uint8_t pubkey[PK_PROTO_PUBKEY_LEN];           /* 0x04 || X(32) || Y(32) */
    pk_proto_session_t vcsec;
    pk_proto_session_t infotainment;
} pk_proto_key_slot_t;

/* 2-byte big-endian bounded streaming BLE frame assembler (D05) */
typedef struct {
    uint8_t buf[PK_PROTO_MAX_FRAME_LEN];
    size_t expected_len;
    size_t received_len;
    uint8_t len_bytes[2];
    uint8_t len_received;
    pk_ms_t last_chunk_ms;
    bool active;
    uint32_t err_zero_len;
    uint32_t err_oversize;
    uint32_t err_timeout;
} pk_rx_framer_t;

/* Bounded GATT fragmented TX & completion-token state machine (N06) */
typedef struct {
    uint32_t conn_gen;
    uint32_t write_seq;
    int      att_status;
} pk_gatt_cb_ev_t;

#define PK_GATT_CB_QUEUE_CAP 4u

typedef struct {
    uint32_t        conn_gen;
    uint32_t        write_seq;
    uint16_t        att_mtu;
    bool            in_flight;
    pk_ms_t         deadline_ms;
    size_t          total_bytes;
    size_t          bytes_sent;
    uint32_t        chunks_sent;
    pk_tx_outcome_t last_outcome;
    pk_gatt_cb_ev_t cb_queue[PK_GATT_CB_QUEUE_CAP];
    uint8_t         cb_head;
    uint8_t         cb_count;
    uint32_t        cb_overflow_drops;
} pk_gatt_tx_t;

void     pk_gatt_tx_init(pk_gatt_tx_t *tx, uint32_t conn_gen, uint16_t negotiated_mtu);
void     pk_gatt_tx_set_mtu(pk_gatt_tx_t *tx, uint16_t negotiated_mtu);
uint16_t pk_gatt_tx_chunk_size(const pk_gatt_tx_t *tx);
pk_ms_t  pk_gatt_tx_remaining_ms(const pk_gatt_tx_t *tx, pk_ms_t now_ms);
bool     pk_gatt_tx_begin(pk_gatt_tx_t *tx, size_t total_bytes, pk_ms_t now_ms, pk_ms_t deadline_ms);
/* Advance one chunk write; returns PK_OK when chunk sent, PK_ERR_EXPIRED on deadline,
 * PK_ERR_OFFLINE on link down, PK_ERR_UNKNOWN on transport error. */
pk_err_t pk_gatt_tx_step(pk_gatt_tx_t *tx, pk_ms_t now_ms, bool link_up,
                         int transport_write_rc, size_t *chunk_len_out);
/* Enqueue an ATT write completion callback; returns PK_ERR_BUSY on queue full (never silent). */
pk_err_t pk_gatt_tx_enqueue_cb(pk_gatt_tx_t *tx, uint32_t conn_gen, uint32_t write_seq, int att_status);
/* Consume one queued callback; ignores stale conn_gen/write_seq (matched_out=false). */
pk_err_t pk_gatt_tx_poll_cb(pk_gatt_tx_t *tx, pk_ms_t now_ms, bool *matched_out);
void     pk_gatt_tx_on_disconnect(pk_gatt_tx_t *tx, uint32_t new_conn_gen);

/* Parsed VCSEC response (FromVCSECMessage) */
typedef struct {
    bool has_vehicle_status;
    bool has_lock_state;
    pk_lock_read_t lock_state;
    bool has_closure_statuses;
    pk_closure_t closures[PK_CLOSURE_COUNT];
    pk_closure_t charge_port;
    int sleep_status; /* 0=unknown, 1=awake, 2=asleep */

    bool has_command_status;
    int op_status;    /* 0=OK, 1=WAIT, 2=ERROR */
    bool has_whitelist_status;
    int whitelist_info;
    int signed_msg_fault;

    bool has_whitelist_entry_info;
    uint32_t entry_key_role;
    uint8_t entry_pubkey_id[20];
    bool has_nominal_error;
    int generic_error;
} pk_proto_vcsec_resp_t;

/* Parsed Infotainment response (CarServer.Response) */
typedef struct {
    bool has_action_status;
    int result; /* 0=OK, 1=ERROR */
    char reason_text[64];

    bool has_vehicle_data;

    /* DriveState */
    bool has_drive_state;
    pk_shift_t shift;
    bool speed_present;
    int speed_mph;
    bool speed_float_present;
    int speed_float_mph;
    bool speed_float_invalid; /* non-finite (NaN/+Inf/-Inf) or out-of-range float */
    bool speed_invalid;       /* invalid or conflicting speed evidence (forbids Park) */
    bool power_present;
    int power_kw;
    bool drive_ts_present;
    int64_t drive_ts_ms;

    /* ClosuresState */
    bool has_closures_state;
    bool locked_present;
    bool locked;
    bool closures_open_present[PK_CLOSURE_COUNT];
    bool closures_open[PK_CLOSURE_COUNT];
    bool sentry_present;
    bool sentry_armed;
    bool remote_start_present;
    bool remote_start;
    bool closures_ts_present;
    int64_t closures_ts_ms;

    /* ChargeState */
    bool has_charge_state;
    int charging_state;
    bool battery_level_present;
    int battery_level;
    bool usable_battery_level_present;
    int usable_battery_level;
    bool battery_range_present;
    int battery_range_tenths_mi;
    bool charging_amps_present;
    int charging_amps;
    bool charge_current_max_present;
    int charge_current_max;
    bool minutes_to_full_present;
    int minutes_to_full;
    bool minutes_to_limit_present;
    int minutes_to_limit;
    bool charge_port_door_open_present;
    bool charge_port_door_open;
    bool cable_connected_present;
    bool cable_connected;
    bool charge_ts_present;
    int64_t charge_ts_ms;

    /* ClimateState */
    bool has_climate_state;
    bool inside_temp_present;
    int inside_temp_tenths_c;
    bool outside_temp_present;
    int outside_temp_tenths_c;
    bool is_climate_on_present;
    bool is_climate_on;
    bool is_preconditioning_present;
    bool is_preconditioning;
    bool steering_heater_present;
    bool steering_heater_on;
    bool seat_heater_left_present;
    int seat_heater_left;
    bool seat_heater_right_present;
    int seat_heater_right;
    bool seat_heater_rl_present;
    int seat_heater_rl;
    bool seat_heater_rr_present;
    int seat_heater_rr;
    bool climate_ts_present;
    int64_t climate_ts_ms;

    /* TirePressureState */
    bool has_tire_pressure_state;
    bool tpms_present[4];
    int tpms_mbar[4];
    bool tpms_ts_present;
    int64_t tpms_ts_ms;
} pk_proto_carserver_resp_t;

/* Parsed outer RoutableMessage (decoded via official nanopb) */
typedef struct {
    bool has_from_domain;
    pk_proto_domain_t from_domain;
    bool has_to_domain;
    pk_proto_domain_t to_domain;
    bool has_to_routing_addr;
    uint8_t to_routing_addr[PK_PROTO_ROUTING_ADDR_LEN];
    bool has_from_routing_addr;
    uint8_t from_routing_addr[PK_PROTO_ROUTING_ADDR_LEN];
    bool has_request_uuid;
    uint8_t request_uuid[PK_PROTO_UUID_LEN];
    bool has_uuid;
    uint8_t uuid[PK_PROTO_UUID_LEN];
    uint32_t flags;

    bool has_msg_status;
    int operation_status;
    int signed_message_fault;

    bool has_session_info;
    uint8_t session_info_bytes[128];
    size_t session_info_len;

    bool has_sig_data;
    uint32_t sig_type_tag; /* Signatures_SignatureData which_sig_type */

    bool has_session_info_tag;
    uint8_t session_info_tag[PK_PROTO_HMAC_LEN];
    size_t session_info_tag_len;

    bool has_aes_gcm_resp;
    uint8_t resp_nonce[PK_PROTO_NONCE_LEN];
    size_t resp_nonce_len;
    uint32_t resp_counter;
    uint8_t resp_tag[PK_PROTO_TAG_LEN];
    size_t resp_tag_len;

    bool has_payload_bytes;
    uint8_t payload_bytes[PK_PROTO_MAX_FRAME_LEN];
    size_t payload_len;
} pk_proto_routable_t;

/* Production RNG & entropy-readiness gate */
void pk_proto_set_entropy_source(uint32_t src_mask, bool active);
bool pk_proto_entropy_ready(void);
bool pk_proto_random_bytes(uint8_t *out, size_t len);
void pk_proto_zeroize(void *buf, size_t len);

/* Cryptographic primitives & helpers */
bool pk_proto_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
bool pk_proto_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
bool pk_proto_hmac_sha256(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[32]);
bool pk_proto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
bool pk_proto_aes128_gcm_encrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plaintext, size_t len,
                                 uint8_t *ciphertext,
                                 uint8_t tag[16]);
bool pk_proto_aes128_gcm_decrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *ciphertext, size_t len,
                                 const uint8_t tag[16],
                                 uint8_t *plaintext);
void pk_proto_aes128_gcm_clear_cache(void);

/* P-256 key generation and ECDH shared key derivation: K = SHA1(Sx)[0..15] */
bool pk_proto_p256_keygen(uint8_t priv_out[32], uint8_t pub_out[65], const uint8_t *seed32);
bool pk_proto_p256_pubkey_from_priv(const uint8_t priv[32], uint8_t pub_out[65]);
bool pk_proto_ecdh_derive_session_key(const uint8_t priv[32],
                                      const uint8_t peer_pub[65],
                                      uint8_t session_key_out[16]);

/* BLE Local Name from 17-char VIN: "S" + hex_lower(SHA1(VIN)[0..7]) + "C" */
bool pk_proto_vin_to_ble_name(const char *vin, char out[PK_PROTO_BLE_NAME_BUF]);

/* Wire framing (2-byte BE length prefix, D05 streaming reassembly) */
void pk_rx_framer_init(pk_rx_framer_t *f);
/* Streaming feed: writes consumed bytes into *consumed_out so callers loop
 * over coalesced frames and keep trailing prefixes across notifications.
 * Returns 1 on complete frame in f->buf[0..f->expected_len-1], 0 when more
 * bytes are needed, -1 on zero-len / oversize / invalid argument.
 * A declared length of 0 or above PK_PROTO_MAX_FRAME_LEN discards the rest
 * of this chunk and resets the framer. The tail is not scanned for a header. */
int pk_rx_framer_feed_ex(pk_rx_framer_t *f, const uint8_t *chunk, size_t chunk_len,
                         pk_ms_t now_ms, size_t *consumed_out);
int pk_rx_framer_feed(pk_rx_framer_t *f, const uint8_t *chunk, size_t chunk_len, pk_ms_t now_ms);
size_t pk_proto_frame_wrap(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t out_cap);

typedef struct {
    const char    *vin;
    const uint8_t *challenge;
    size_t         challenge_len;
    const uint8_t *encoded_info;
    size_t         encoded_info_len;
} pk_proto_session_info_msg_t;

typedef struct {
    pk_proto_session_info_msg_t info;
    const uint8_t              *privkey; /* [PK_PROTO_PRIVKEY_LEN] */
    const uint8_t              *tag;
    size_t                      tag_len;
    pk_ms_t                     now_ms;
} pk_proto_session_apply_t;

typedef struct {
    pk_proto_domain_t domain;
    const uint8_t    *routing_addr; /* [PK_PROTO_ROUTING_ADDR_LEN] */
    const uint8_t    *uuid;         /* [PK_PROTO_UUID_LEN] */
} pk_proto_route_t;

typedef struct {
    pk_proto_route_t  route;
    const char       *vin;
    const uint8_t    *client_pubkey; /* [PK_PROTO_PUBKEY_LEN] */
    const uint8_t    *inner_payload;
    size_t            inner_len;
    const uint8_t    *nonce;         /* [PK_PROTO_NONCE_LEN] */
    pk_ms_t           now_ms;
    uint32_t          ttl_seconds;
} pk_proto_signed_req_t;

typedef struct {
    const pk_proto_session_t *sess;
    const char               *vin;
    const uint8_t            *req_id; /* [17] */
} pk_proto_decrypt_t;

/* SessionInfo parsing & HMAC verification */
bool pk_proto_session_info_tag_compute(const uint8_t session_key[16],
                                       const pk_proto_session_info_msg_t *msg,
                                       uint8_t tag_out[32]);
bool pk_proto_session_apply_info(pk_proto_session_t *sess,
                                 const pk_proto_session_apply_t *in);

/* Inner payload encoders (official nanopb) */
size_t pk_proto_encode_vcsec_rke(pk_proto_rke_action_t rke, uint8_t *out, size_t cap);
size_t pk_proto_encode_vcsec_closure(int closure_id, pk_proto_closure_move_t move, uint8_t *out, size_t cap);
size_t pk_proto_encode_vcsec_info_req(pk_proto_info_req_t req_type, uint8_t *out, size_t cap);
size_t pk_proto_encode_vcsec_add_key(const uint8_t pubkey[65],
                                     pk_proto_role_t role,
                                     pk_proto_key_form_t form_factor,
                                     uint8_t *out, size_t cap);

size_t pk_proto_encode_carserver_get_data(uint32_t category_mask, uint8_t *out, size_t cap);
#define PK_CARSERVER_DATA_CHARGE   (1u << 0)
#define PK_CARSERVER_DATA_CLIMATE  (1u << 1)
#define PK_CARSERVER_DATA_DRIVE    (1u << 2)
#define PK_CARSERVER_DATA_CLOSURES (1u << 3)
#define PK_CARSERVER_DATA_TIRES    (1u << 4)

#define PK_STATUS_SECTION_COUNT 5
size_t pk_proto_encode_status_section(int section, uint8_t *out, size_t cap);

size_t pk_proto_encode_action_payload(pk_action_t action,
                                      const pk_draft_t *draft,
                                      const uint8_t *client_pubkey65,
                                      uint8_t *out, size_t cap);

/* RoutableMessage builders (official nanopb) */
size_t pk_proto_build_session_info_request(const pk_proto_route_t *route,
                                           const uint8_t client_pubkey[65],
                                           uint8_t *out, size_t cap);

size_t pk_proto_build_unsigned_routable(const pk_proto_route_t *route,
                                        const uint8_t *inner_payload, size_t inner_len,
                                        uint8_t *out, size_t cap);

size_t pk_proto_build_signed_routable(const pk_proto_signed_req_t *req,
                                      pk_proto_session_t *sess,
                                      uint8_t req_id_out[17],
                                      uint8_t *out, size_t cap);

/* Parsers for incoming BLE frames (official nanopb) */
bool pk_proto_parse_routable(const uint8_t *buf, size_t len, pk_proto_routable_t *out);

bool pk_proto_decrypt_routable_payload(const pk_proto_routable_t *msg,
                                       const pk_proto_decrypt_t *ctx,
                                       uint8_t *plain_out, size_t plain_cap,
                                       size_t *plain_len_out);
bool pk_proto_parse_vcsec_response(const uint8_t *buf, size_t len, pk_proto_vcsec_resp_t *out);
bool pk_proto_parse_carserver_response(const uint8_t *buf, size_t len, pk_proto_carserver_resp_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PK_PROTOCOL_H */
