#ifndef PK_TYPES_H
#define PK_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pk_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t pk_ms_t;
typedef uint32_t pk_id_t;

/* Wrap-safe monotonic timestamp helpers (valid for deltas < 2^31 ms ~ 24.8 days). */
static inline bool pk_time_before(pk_ms_t a, pk_ms_t b)
{
    return (int32_t)(a - b) < 0;
}

static inline bool pk_time_due(pk_ms_t now, pk_ms_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static inline bool pk_time_elapsed(pk_ms_t now, pk_ms_t start, pk_ms_t dur)
{
    return (uint32_t)(now - start) >= dur;
}

typedef enum {
    PK_OK = 0,
    PK_ERR_REJECTED,
    PK_ERR_EXPIRED,
    PK_ERR_MISMATCH,      /* in-flight scope / txn did not match */
    PK_ERR_BUSY,
    PK_ERR_OFFLINE,
    PK_ERR_UNSUPPORTED,
    PK_ERR_TIMEOUT,
    PK_ERR_CANCELED,
    PK_ERR_INVALID,
    PK_ERR_NOT_WIRED,     /* hardware or SDK port is not attached; not a vehicle result */
    PK_ERR_BLOCKED,       /* guard / management / qualification blocked */
    PK_ERR_UNKNOWN
} pk_err_t;

/* Transport-layer transmission outcome (S2 §2B):
 *   PK_TX_NOT_SENT:     definitely 0 bytes reached BLE GATT;
 *   PK_TX_SENT_UNKNOWN: >= 1 chunk was handed to BLE GATT without confirmed delivery;
 *   PK_TX_DELIVERED:    all chunks acknowledged by matching GATT write-with-response cb. */
typedef enum {
    PK_TX_NOT_SENT = 0,
    PK_TX_SENT_UNKNOWN = 1,
    PK_TX_DELIVERED = 2
} pk_tx_outcome_t;

/* Tri-state capability. Defaults are UNKNOWN. Missing evidence forbids execution. */
typedef enum {
    PK_CAP_UNKNOWN = 0,
    PK_CAP_NO,
    PK_CAP_YES
} pk_cap_t;

typedef enum {
    PK_KEY_NONE = 0,
    PK_KEY_UP,      /* ▲ */
    PK_KEY_DOT,     /* ● */
    PK_KEY_DOWN,    /* ▼ */
    PK_KEY_POWER    /* left independent power key */
} pk_key_id_t;

/*
 * The application accepts only raw edges. Adapters MUST NOT feed
 * BUTTON_SINGLE_CLICK / BUTTON_DOUBLE_CLICK / BUTTON_LONG_PRESS_START
 * or any other BSP cooked event (180 ms click window / 500 ms long-press).
 */
typedef enum {
    PK_EDGE_PRESS = 0,
    PK_EDGE_RELEASE
} pk_edge_t;

typedef enum {
    PK_PAGE_MAIN = 0,
    PK_PAGE_CARS,
    PK_PAGE_SETTINGS,
    PK_PAGE_STATUS,
    PK_PAGE_WAIT,
    PK_PAGE_RESULT,
    PK_PAGE_OFFLINE,
    PK_PAGE_VIN,
    PK_PAGE_PAIR,
    PK_PAGE_LOGS,
    PK_PAGE_QUALIFICATION_OFFER,
    PK_PAGE_DELETE_CONFIRM,
    PK_PAGE_RESET_CONFIRM
} pk_page_t;

typedef enum {
    PK_ORIGIN_LIST = 0,
    PK_ORIGIN_DRIVING
} pk_origin_t;

typedef enum {
    PK_DOMAIN_VCSEC = 0,
    PK_DOMAIN_INFOTAINMENT
} pk_domain_t;

typedef enum {
    PK_ACTION_NONE = 0,
    PK_ACTION_LOCK,
    PK_ACTION_UNLOCK,
    PK_ACTION_FRUNK_OPEN,
    PK_ACTION_TRUNK_MOVE,
    PK_ACTION_CLIMATE_ON,
    PK_ACTION_PRECONDITION_MAX_ON,
    PK_ACTION_CHARGE_PORT_OPEN,
    PK_ACTION_CHARGE_PORT_CLOSE,
    PK_ACTION_WINDOW_VENT,
    PK_ACTION_FLASH,
    PK_ACTION_HONK,
    PK_ACTION_SET_SENTRY,
    PK_ACTION_SET_SEAT_HEAT,
    PK_ACTION_SET_STEERING_HEAT,
    PK_ACTION_GET_QUALIFICATION,
    PK_ACTION_VCSEC_WAKE_ONCE,
    PK_ACTION_VCSEC_GET_STATUS_ONCE,
    PK_ACTION_STATUS_READ,
    PK_ACTION_ADDKEY,
    PK_ACTION_SELECT,
    PK_ACTION_RETURN_MAIN,
    PK_ACTION_RECONNECT,
    PK_ACTION_NO_TARGET,
    PK_ACTION_PAIR_VERIFY_VCSEC,
    PK_ACTION_PAIR_VERIFY_INFOTAINMENT
} pk_action_t;

typedef enum {
    PK_PENDING_NONE = 0,
    PK_PENDING_COMMAND,
    PK_PENDING_QUALIFICATION,
    PK_PENDING_LOCK_VERIFY,
    PK_PENDING_UNLOCK_VERIFY,
    PK_PENDING_STATUS,
    PK_PENDING_WAKE_THEN
} pk_pending_kind_t;

typedef enum {
    PK_QPHASE_NONE = 0,
    PK_QPHASE_WAKE_WAIT,
    PK_QPHASE_READ_WAIT
} pk_qualify_phase_t;

typedef enum {
    PK_LOCK_FSM_IDLE = 0,
    PK_LOCK_FSM_LOCK_PENDING,
    PK_LOCK_FSM_VERIFY,
    PK_LOCK_FSM_RESULT
} pk_lock_fsm_t;

typedef enum {
    PK_CMD_RESULT_NONE = 0,
    PK_CMD_RESULT_WAIT,          /* intermediate; not terminal accept */
    PK_CMD_RESULT_ACCEPTED,
    PK_CMD_RESULT_REJECTED,
    PK_CMD_RESULT_UNCONFIRMED,
    PK_CMD_RESULT_TIMEOUT
} pk_cmd_result_t;

typedef enum {
    PK_LOCK_READ_UNKNOWN = 0,
    PK_LOCK_READ_LOCKED,
    PK_LOCK_READ_UNLOCKED,
    PK_LOCK_READ_INTERNAL_LOCKED,
    PK_LOCK_READ_SELECTIVE_UNLOCKED
} pk_lock_read_t;

typedef enum {
    PK_CLOSURE_UNKNOWN = 0,
    PK_CLOSURE_CLOSED,
    PK_CLOSURE_OPEN,
    PK_CLOSURE_AJAR,
    PK_CLOSURE_OPENING,
    PK_CLOSURE_CLOSING,
    PK_CLOSURE_FAILED_UNLATCH
} pk_closure_t;

typedef enum {
    PK_CLOSURE_FRONT_LEFT = 0,
    PK_CLOSURE_FRONT_RIGHT,
    PK_CLOSURE_REAR_LEFT,
    PK_CLOSURE_REAR_RIGHT,
    PK_CLOSURE_FRONT,
    PK_CLOSURE_REAR,
    PK_CLOSURE_COUNT
} pk_closure_id_t;

typedef enum {
    PK_OFFER_CHECK = 0,
    PK_OFFER_WAKE,
    PK_OFFER_CANCEL,
    PK_OFFER_BACK
} pk_offer_focus_t;

typedef enum {
    PK_POWER_COMFORT = 0, /* 体验优先 */
    PK_POWER_ENDURANCE    /* 续航优先 */
} pk_power_profile_t;

typedef enum {
    PK_SETTING_BRIGHTNESS = 0,
    PK_SETTING_POWER,
    PK_SETTING_FLASH,
    PK_SETTING_HORN,
    PK_SETTING_VENT,
    PK_SETTING_CLIMATE,
    PK_SETTING_SENTRY,
    PK_SETTING_DEFOG,
    PK_SETTING_PORT,
    PK_SETTING_SEAT,
    PK_SETTING_STEERING,
    PK_SETTING_LOGS,
    PK_SETTING_HOTSPOT,
    PK_SETTING_RESET,
    PK_SETTING_COUNT
} pk_setting_id_t;

typedef enum {
    PK_LOG_EVENT_UNKNOWN = 0,
    PK_LOG_EVENT_COMMAND_INTENT,
    PK_LOG_EVENT_QUERY,
    PK_LOG_EVENT_PAIR,
    PK_LOG_EVENT_CONNECT,
    PK_LOG_EVENT_DELETE,
    PK_LOG_EVENT_RESET,
    PK_LOG_EVENT_POWER
} pk_log_event_t;

typedef enum {
    PK_LOG_ERR_NONE = 0,
    PK_LOG_ERR_TIMEOUT,
    PK_LOG_ERR_REJECTED,
    PK_LOG_ERR_UNSUPPORTED,
    PK_LOG_ERR_OFFLINE,
    PK_LOG_ERR_UNKNOWN,
    PK_LOG_ERR_CANCELED
} pk_log_err_t;

typedef enum {
    PK_SHIFT_ABSENT = 0,
    PK_SHIFT_P,
    PK_SHIFT_D,
    PK_SHIFT_R,
    PK_SHIFT_N,
    PK_SHIFT_INVALID
} pk_shift_t;

typedef enum {
    PK_MGMT_SWITCH = 0,
    PK_MGMT_ADD,
    PK_MGMT_PAIR,
    PK_MGMT_DELETE,
    PK_MGMT_RESET
} pk_mgmt_op_t;

#define PK_VIN_LEN        17
#define PK_VIN_BUF        18
#define PK_SLOT_ID_LEN    12
#define PK_MSG_LEN        160
#define PK_VEHICLE_NONE            (-1)
/* Ephemeral radio target for an unsaved pairing VIN. Not a store slot
 * (store slots are 0 .. PK_MAX_VEHICLES-1). connect_once(NONE) stays
 * invalid; first-pair and pair-another-car writes use this candidate
 * instead of the current saved vehicle. Never assign it to app->vehicle
 * and never persist it in NVS.
 */
#define PK_VEHICLE_PAIR_CANDIDATE  (PK_MAX_VEHICLES)

#ifdef __cplusplus
}
#endif

#endif /* PK_TYPES_H */
