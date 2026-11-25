#ifndef PK_APP_H
#define PK_APP_H

#include "pk_copy.h"
#include "pk_hal.h"
#include "pk_log.h"
#include "pk_scope.h"
#include "pk_store.h"
#include "pk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PK_FIELD_CAP  16
#define PK_FIELD_NAME 24
#define PK_SEAT_CAP   8

typedef struct {
    char name[PK_FIELD_NAME];
    bool present;
    bool unit_verified;
    bool clock_verified;
    bool has_value;
    int value;
    bool sample_present;
    int64_t sampled;
    pk_ms_t read_at;
    /* 未知 / 本次读取窗口 / 陈旧 / 时间待确认 */
    char timeliness[24];
} pk_status_field_t;

typedef struct {
    uint8_t action[64];    /* pk_cap_t per action; use helpers */
    pk_cap_t cable_connected;
    pk_cap_t car_wash;
    pk_cap_t climate_on;
    pk_cap_t preconditioning; /* HvacSetPreconditioningMax state, when reported */
    pk_cap_t keeper_on;
    pk_cap_t pair_unknown_form_supported;
    pk_cap_t port_open;
    bool sentry_known;
    bool sentry_armed;
    int seat_positions[PK_SEAT_CAP];
    int seat_position_count;
    bool clock_verified;     /* default false; production gate */
    bool adapter_verified;   /* default false; production gate */
} pk_caps_t;

typedef struct {
    pk_ms_t down_at;
    bool down;
    pk_ms_t deadline;
    bool deadline_valid;
    bool second;
    bool consumed;
    bool has_intent;
    pk_intent_t intent;
    bool exit_cycle;
    bool wake_cycle;
} pk_arbiter_t;

typedef struct {
    pk_key_id_t key;
    bool active;
    pk_ms_t at;
    bool consumed;
    pk_intent_t intent;
} pk_side_t;

typedef struct {
    pk_pending_kind_t kind;
    pk_action_t action;
    pk_id_t id;
    pk_scope_t scope;
    pk_ms_t requested;
    pk_ms_t deadline;
    bool wake;
    pk_qualify_phase_t qphase;
    unsigned shards;
    pk_cmd_result_t command_result;
    pk_cap_t bool_enabled; /* frozen toggle direction for the command */
} pk_pending_t;

typedef struct {
    unsigned attempt;
    unsigned max_attempts;
    pk_ms_t deadline;
    pk_ms_t next;
    bool active;
} pk_reconnect_t;

typedef struct {
    pk_setting_id_t key;
    bool valid;
    int amps;
    int seat_position;
    int seat_level;
    pk_cap_t bool_enabled;
    int brightness;
    pk_power_profile_t power;
    int vehicle;
    uint32_t connection;
    uint32_t session;
} pk_draft_t;

/* Known motion tendency is a fresh trusted D/R/N or finite non-zero speed.
 * Qualified park lives on the grant, not on moving==false. Anything else,
 * including disconnect and an expired sample, is unknown — not "stopped". */
typedef enum {
    PK_MOTION_UNKNOWN = 0,
    PK_MOTION_TENDENCY = 1
} pk_motion_t;

enum {
    PK_WAKE_OWNER_NONE = 0,
    PK_WAKE_OWNER_STATUS = 1,
    PK_WAKE_OWNER_COMMAND = 2,
    PK_WAKE_OWNER_QUALIFY = 3
};

typedef struct pk_app {
    pk_ms_t now;
    pk_ms_t last_activity;
    bool time_set;

    int vehicle;           /* current slot or PK_VEHICLE_NONE */
    bool paired;
    uint32_t connection;
    uint32_t session;
    uint32_t generation;
    bool connected;
    bool busy;
    bool awake;
    /* True only while a fresh trusted snapshot shows a known motion tendency.
     * False is not a proof that the car has stopped. */
    bool moving;
    pk_motion_t motion;
    pk_ms_t motion_until;
    pk_page_t page;
    pk_page_t modal_return_page; /* page a WAIT/RESULT takeover interrupted */
    pk_origin_t origin;
    pk_setting_id_t focus;
    pk_msg_id_t message;
    char message_extra[PK_MSG_LEN]; /* lock-verify composed text only; no secrets */

    pk_grant_t grant;
    bool has_grant;
    pk_pending_t pending;
    bool has_pending;
    pk_id_t serial;
    pk_lock_fsm_t lock_fsm;

    pk_log_t logs;
    pk_status_field_t fields[PK_FIELD_CAP];
    unsigned field_count;
    /* One VehicleData section per request; index into PK_STATUS_SECTION_COUNT. */
    uint8_t status_step;
    /* Sections the vehicle rejected with fault 25 during this page read. */
    uint8_t status_skip_mask;
    /* Learned fault-25 exclusions for the current connection/session. */
    uint8_t status_too_large_mask;
    /* Set from the BLE RX path; the app task sends the next section. */
    bool status_need_send;
    char status_phase[16];
    /* Silent capability probe: no page change, no status fields collected. */
    bool status_silent;
    pk_page_t status_return_page;
    /* Status page entry asked for a wake first; resume the read after settle. */
    bool wake_then_status;
    /* One wake per intent. Not cleared by page re-entry or reconnect. */
    uint8_t wake_owner;
    bool wake_spent;
    /* AddKey asks for Driver. That request is not a whitelist read. */
    bool addkey_requests_driver;
    bool role_observed;
    bool role_is_driver;
    bool identity_binding_verified;
    bool session_vcsec_proven;
    bool session_infotainment_proven;

    pk_caps_t caps;
    pk_draft_t draft;
    bool editing;
    int editor_index;

    pk_msg_id_t auth;
    pk_ms_t auth_until;

    bool ap;
    pk_ms_t ap_deadline;
    char vin[PK_VIN_BUF];
    bool has_vin;
    int pair_step;
    pk_id_t pair_id;
    bool enrollment_sent;
    bool approved;
    bool orphan;
    pk_ms_t pair_deadline;
    pk_scope_t pair_scope;   /* unsaved pairing target; not the current car */
    bool pair_radio_held;    /* adapter is temporarily on the pairing candidate */

    int brightness;
    pk_power_profile_t power;

    pk_reconnect_t reconnect;
    pk_setting_id_t edit_key;
    bool edit_open;
    bool settings_quick_mode;
    pk_draft_t recheck;
    bool has_recheck;
    /* Park-gated action frozen at key-down; replayed once a check-before-act
     * drive-state read proves P. Never sent without that proof. */
    pk_intent_t deferred;
    bool has_deferred;
    /* Absolute end of the open wake or command, fixed when that work is
     * accepted. A pure status wake uses the same ceiling as a command wake.
     * Later settle, qualification, read, and the first submit may use a
     * shorter phase slice. They must not move this instant later.
     * 0 means no wake or command total is open. */
    pk_ms_t intent_deadline;
    pk_offer_focus_t offer_focus;
    bool wake_choice_available;

    pk_arbiter_t keys;
    pk_side_t side;

    bool battery_known;
    int battery_percent;   /* only if battery_known; never display unknown as 0% */
    bool low_battery;
    bool desktop;          /* USB host/desk power input; never deep sleep */

    int car_focus;         /* list focus: slot, or -10 add, -11 status, -12 settings */
    int delete_target;
    int delete_focus;      /* 0 cancel (default), 1 confirm */
    int reset_focus;
    int log_focus;

    pk_store_t store;
    pk_hal_t hal;

    /* First complete key cycle after wake: consume only, no vehicle command. */
    bool consume_as_wake_only;

    /* VCSEC native unlock snapshot (avoids waking Infotainment for UNLOCK). */
    /* Authenticated VCSEC status snapshot (may feed unlock / wake logic). */
    bool           vcsec_status_valid;
    uint8_t        vcsec_reconcile_attempts;
    pk_ms_t        vcsec_reconcile_next_ms;
    bool           vcsec_asleep;
    pk_lock_read_t vcsec_lock_state;
    /* Non-authoritative Legacy observation for display ONLY (S2 §1).
     * MUST NEVER feed into pk_app_can_vcsec_unlock(), Park qualification,
     * lock confirmation, or any security/authorization derivation. */
    bool           vcsec_observed_valid;
    bool           vcsec_observed_asleep;
    pk_lock_read_t vcsec_observed_lock_state;
    /* Last transport write outcome preserved for orchestration/S3 (S2 §2B). */
    pk_tx_outcome_t last_tx_outcome;

    /* Clean lock/unlock confirmation auto-return to main screen. */
    bool           result_auto_return;
    pk_ms_t        result_return_until;

    /* Full-brightness closure alert. A dropped LOCK is not relayed. */
    bool    lock_alert_active;
    pk_ms_t lock_alert_until;
} pk_app_t;

void pk_app_init(pk_app_t *app, int initial_vehicle /* PK_VEHICLE_NONE or 0..2 */, bool paired);
void pk_hal_attach(pk_app_t *app, const pk_hal_t *hal); /* still may be unwired */

/* Monotonic watermark. Same-timestamp order: invalidate first, then key
 * edges, then committable timers. tick(deadline) never commits a single
 * click; a second PRESS at t <= deadline wins; t > deadline may commit.
 */
pk_err_t pk_app_tick(pk_app_t *app, pk_ms_t now);
pk_err_t pk_app_button_edge(pk_app_t *app, pk_ms_t now, pk_key_id_t key, pk_edge_t edge);

typedef struct {
    pk_id_t     request_id;
    pk_scope_t  scope;
    pk_shift_t  shift;
    bool        speed_present;
    int         speed;
    bool        speed_float_present;
    int         speed_float;
    bool        speed_invalid; /* non-finite / negative / conflicting speed forbids Park.
                                * It does not erase a trusted D/R/N or a finite non-zero speed. */
    bool        locked_present;
    bool        locked;
    bool        sample_present;
    int64_t     sample;
    bool        lock_sample_present;
    int64_t     lock_sample;
    bool        clock_verified;
    bool        adapter_verified;
} pk_qualify_sample_t;

/* Map one decoded drive snapshot into the qualification sample.
 * A live authenticated drive record stays present when speed is illegal.
 * speed_invalid remains set so Park cannot be granted from that record. */
void pk_qualify_fill_drive(pk_qualify_sample_t *qs, pk_id_t request_id,
                           const pk_scope_t *scope, bool has_drive, pk_shift_t shift,
                           bool speed_present, int speed,
                           bool speed_float_present, int speed_float,
                           bool speed_invalid, bool locked_present, bool locked,
                           bool crypto_auth, int64_t now);

typedef struct {
    pk_id_t             request_id;
    bool                authenticated;
    bool                correlated;
    bool                adapter_verified;
    pk_lock_read_t      lock;
    const pk_closure_t *closures;
    bool                closures_complete;
    bool                complete;
} pk_lock_packet_t;

typedef struct {
    pk_id_t           request_id;
    const pk_scope_t *scope;
    const char       *name;
    int               value;
    bool              present;
    bool              sample_present;
    int64_t           sampled;
    bool              unit_verified;
    bool              clock_verified;
} pk_status_input_t;

typedef struct {
    pk_id_t request_id;
    bool    vcsec_authenticated;
    bool    infotainment_authenticated;
    bool    key_matches;
    /* role_driver is meaningful only when role_observed is true. A dual-domain
     * handshake must not set either flag. */
    bool    role_observed;
    bool    role_driver;
    bool    identity_binding_verified;
} pk_pair_verify_t;

/* Injected results from a future transport. All must match in-flight scope + txn. */
bool pk_app_qualification_wake_result(pk_app_t *app, pk_id_t request_id, const pk_scope_t *scope,
                                      bool accepted, bool authenticated);
/* S2 §1: Bounded legacy wake ACK under an already-authorized qualification wake:
 * advances once to the read-only qualification query without increasing wake
 * budget, extending deadline, or triggering any write intent. */
bool pk_app_qualification_wake_legacy_ack(pk_app_t *app, pk_id_t request_id,
                                          const pk_scope_t *scope);
bool pk_app_qualification_result(pk_app_t *app, const pk_qualify_sample_t *sample);
bool pk_app_command_result(pk_app_t *app, pk_id_t op_id, pk_cmd_result_t result, bool authenticated);
/* S2 §1: Compliant Legacy plaintext receipt for a live in-flight command.
 * Surfaces honest unconfirmed status without claiming vehicle execution or
 * mutating capability state. */
bool pk_app_command_legacy_receipt(pk_app_t *app, pk_id_t op_id, pk_cmd_result_t result);
bool pk_app_lock_packet(pk_app_t *app, const pk_lock_packet_t *pkt);
bool pk_app_status_field(pk_app_t *app, const pk_status_input_t *in);
bool pk_app_pair_approve(pk_app_t *app, bool value, pk_id_t request_id, bool correlated);
bool pk_app_pair_verify(pk_app_t *app, const pk_pair_verify_t *v);

void pk_app_set_connected(pk_app_t *app, bool connected);
/* Desktop/USB-host presence; does not persist and does not grant a vehicle action. */
void pk_app_set_desktop(pk_app_t *app, bool desktop);
void pk_app_vcsec_status(pk_app_t *app, bool asleep, pk_lock_read_t lock_state);
/* S2 §1: Non-authoritative Legacy VCSEC observation; updates display-only fields
 * and never feeds security/authorization or confirms lock/unlock. */
void pk_app_vcsec_status_observed(pk_app_t *app, bool asleep, pk_lock_read_t lock_state);
/* Vehicle fault (UniversalMessage.MessageFault_E). Authenticated=false denotes
 * an outer unprotected fault (weak fault / resync hint; never presented as
 * cryptographically authenticated rejection, preserves unknown outcome). */
void pk_app_protocol_fault_ex(pk_app_t *app, int fault, bool from_vcsec, bool authenticated);
void pk_app_protocol_fault(pk_app_t *app, int fault, bool from_vcsec);
/* S2 §2B: Notify orchestration when a queued/flushed transport write fails so
 * pending state does not hang and SentUnknown vs NotSent evidence is preserved. */
bool pk_app_tx_failed(pk_app_t *app, pk_id_t txn_id, pk_tx_outcome_t outcome);
void pk_app_ble_slots_full(pk_app_t *app);
void pk_app_cancel_reconnect(pk_app_t *app);
void pk_app_battery(pk_app_t *app, bool known, int percent, bool low);
void pk_app_reboot(pk_app_t *app);

/* Capabilities default to UNKNOWN. Synthetic fixtures must not be compiled
 * into the production path (C12). A later test harness may set these
 * explicitly; the firmware default is unproven.
 */
void pk_app_caps_clear(pk_caps_t *c);

pk_scope_t pk_app_scope(const pk_app_t *app);
bool pk_app_should_deep_sleep(const pk_app_t *app);
bool pk_app_parked(const pk_app_t *app);
/* Parked AND the proof was received within PK_PARK_FRESH_MS. Park-gated
 * commands require this; a stale lease must trigger a fresh drive read. */
bool pk_app_park_fresh(const pk_app_t *app);
bool pk_app_can_vcsec_unlock(const pk_app_t *app);
bool pk_app_management_allowed(const pk_app_t *app, pk_mgmt_op_t op, bool current,
                               pk_msg_id_t *why_out);

/* VIN helper: trim + ASCII upper, 17 chars [A-HJ-NPR-Z0-9], no I/O/Q.
 * Regional check-digit is not applied globally.
 */
bool pk_vin_valid(const char *text, char out[PK_VIN_BUF]);

#ifdef __cplusplus
}
#endif

#endif /* PK_APP_H */
