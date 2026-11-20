#include "pk_app_internal.h"
#include "pk_plan.h"

#include <string.h>

/*
 * M6: post-LOCK verification. Never walks the M4 wake path.
 *
 * IDLE → one LOCK (LOCK_PENDING, finite planner command deadline) → VERIFY
 * (only if the same connection and session are still valid) → one
 * GET_STATUS → RESULT. T_VERIFY = 3000 ms starts when the command phase
 * ends and VERIFY is about to begin. Mid-packets do not extend it.
 *
 * No WAKE_VEHICLE, no Infotainment handshake, no automatic retry, no
 * reconnect just to finish verification, no "全车安全", no reverse-unlock
 * grant, no P / motion qualification from this read.
 *
 * proto3 regular enums have no per-field presence: CLOSED=0, UNLOCKED=0.
 * A missing message, missing closureStatuses, or a local default must not
 * be treated as "all closed". INTERNAL_LOCKED / SELECTIVE_UNLOCKED /
 * unknown enums must not be presented as LOCKED.
 *
 * Charge-port open is not a closure fault. Windows, TPMS, location, and
 * Infotainment are out of this result. Audio remains a hidden independent
 * gate and is not implemented.
 *
 * 待真机: message shape, default-value semantics, left/right door names,
 * software-version fallback.
 */

static const char *k_closure_name[PK_CLOSURE_COUNT] = {
    /* 待真机: market-specific driver-side naming is not verified. */
    "前左", "前右", "后左", "后右", "前盖", "后盖"
};

bool pk_app_start_lock_verification(pk_app_t *app, pk_cmd_result_t command_result)
{
    pk_pending_t *p;

    if (!app) {
        return false;
    }
    /* Command phase is over. Release the LOCK one-shot slot so the
     * subsequent GET_STATUS write is not rejected as BUSY.
     */
    pk_ble_release_in_flight(app);
    if (!app->connected || app->vehicle == PK_VEHICLE_NONE) {
        app->has_pending = false;
        pk_app_set_busy(app, false);
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        pk_app_present_modal(app, PK_PAGE_RESULT);
        app->lock_alert_active = true;
        app->lock_alert_until = app->now + PK_LOCK_ALERT_DISPLAY_MS;
        app->awake = true;
        app->last_activity = app->now;
        pk_app_set_msg(app, PK_MSG_LOCK_NO_LINK);
        return false;
    }

    app->serial++;
    pk_app_set_busy(app, true);
    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_LOCK_VERIFY;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    /* N08: separate post-write, same-connection, single VCSEC read.
     * 3000 ms from verification start. Do not clip it to the command
     * total. Do not wake, retransmit, or reconnect to finish it. */
    p->deadline = app->now + pk_plan_deadline(PK_PLAN_LOCK_VERIFY);
    app->intent_deadline = 0;
    p->shards = 0;
    p->command_result = command_result;
    p->action = PK_ACTION_VCSEC_GET_STATUS_ONCE;
    p->wake = false; /* verification must never request a wake */
    app->has_pending = true;
    app->lock_fsm = PK_LOCK_FSM_VERIFY;
    {
        pk_err_t rc = pk_ble_send_once(app, PK_DOMAIN_VCSEC,
                                       PK_ACTION_VCSEC_GET_STATUS_ONCE, p->deadline);
        pk_tx_outcome_t outcome = pk_app_note_send(app, rc);
        if (rc != PK_OK) {
            pk_ble_release_in_flight(app);
            app->has_pending = false;
            pk_app_set_busy(app, false);
            app->lock_fsm = PK_LOCK_FSM_RESULT;
            pk_app_set_msg(app, (outcome == PK_TX_SENT_UNKNOWN) ? PK_MSG_LOCK_UNCONFIRMED
                                                                 : PK_MSG_CANCELLED);
            return false;
        }
        if (outcome == PK_TX_DELIVERED) {
            pk_app_set_msg(app, PK_MSG_LOCK_VERIFYING);
        }
    }
    pk_app_log(app, PK_LOG_EVENT_QUERY, PK_LOG_ERR_NONE);
    return true;
}

static void format_abnormal_closures(const pk_closure_t *closures, bool closures_complete,
                                     char *abnormal, size_t cap, bool *unknown_out)
{
    unsigned n_abn = 0;
    unsigned written = 0;
    int i;

    if (!abnormal || cap == 0) {
        if (unknown_out) {
            *unknown_out = true;
        }
        return;
    }
    abnormal[0] = '\0';
    *unknown_out = false;
    if (!closures || !closures_complete) {
        *unknown_out = true;
        return;
    }
    for (i = 0; i < PK_CLOSURE_COUNT; i++) {
        pk_closure_t v = closures[i];
        if (v == PK_CLOSURE_OPEN || v == PK_CLOSURE_AJAR ||
            v == PK_CLOSURE_OPENING || v == PK_CLOSURE_CLOSING ||
            v == PK_CLOSURE_FAILED_UNLATCH) {
            n_abn++;
        } else if (v != PK_CLOSURE_CLOSED) {
            *unknown_out = true;
        }
    }
    if (n_abn == 0) {
        return;
    }
    if (n_abn > 2) {
        strncpy(abnormal, pk_copy(PK_MSG_CLOSURES_MANY), cap - 1);
        abnormal[cap - 1] = '\0';
        return;
    }
    for (i = 0; i < PK_CLOSURE_COUNT; i++) {
        if (closures[i] != PK_CLOSURE_OPEN && closures[i] != PK_CLOSURE_AJAR &&
            closures[i] != PK_CLOSURE_OPENING && closures[i] != PK_CLOSURE_CLOSING &&
            closures[i] != PK_CLOSURE_FAILED_UNLATCH) {
            continue;
        }
        if (written) {
            strncat(abnormal, "／", cap - strlen(abnormal) - 1);
        }
        strncat(abnormal, k_closure_name[i], cap - strlen(abnormal) - 1);
        strncat(abnormal, "未关", cap - strlen(abnormal) - 1);
        written++;
    }
}

bool pk_app_lock_packet(pk_app_t *app, const pk_lock_packet_t *pkt)
{
    char abnormal[PK_MSG_LEN];
    char composed[PK_MSG_LEN];
    bool unknown = false;
    pk_msg_id_t lock_msg;
    pk_scope_t cur;

    if (!app || !app->has_pending || !pkt) {
        return false;
    }
    cur = pk_app_scope(app);
    if (app->pending.kind != PK_PENDING_LOCK_VERIFY ||
        pkt->request_id != app->pending.id ||
        pk_time_due(app->now, app->pending.deadline) ||
        !pkt->authenticated || !pkt->correlated ||
        !pk_scope_same_vehicle_conn_session(&app->pending.scope, &cur)) {
        return false;
    }

    app->pending.shards++;
    if (!pkt->complete && app->pending.shards < 3) {
        return false; /* incomplete fragments only; still bound by the deadline */
    }

    /* Top-level object must be a correctly parsed, correctly sourced
     * VehicleStatus inside the window. CommandStatus, an empty success
     * ack, or a default object after a parse failure is not a status.
     */
    if (pkt->complete && pkt->adapter_verified && pkt->lock == PK_LOCK_READ_LOCKED) {
        lock_msg = PK_MSG_LOCK_CONFIRMED;
    } else if (pkt->complete && pkt->adapter_verified && pkt->lock == PK_LOCK_READ_UNLOCKED) {
        lock_msg = PK_MSG_LOCK_UNLOCKED;
    } else {
        /* INTERNAL_LOCKED / SELECTIVE_UNLOCKED / unknown / unverified */
        lock_msg = PK_MSG_LOCK_UNCONFIRMED;
    }

    if (pkt->complete && pkt->adapter_verified) {
        app->vcsec_status_valid = true;
        app->vcsec_lock_state = pkt->lock;
    }

    format_abnormal_closures(pkt->closures, pkt->closures_complete, abnormal, sizeof(abnormal), &unknown);
    pk_copy_lock_result(composed, sizeof(composed), lock_msg,
                        abnormal[0] ? abnormal : NULL, unknown);
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false);
    app->result_auto_return = false;
    if (lock_msg != PK_MSG_LOCK_CONFIRMED || abnormal[0] != '\0') {
        pk_app_present_modal(app, PK_PAGE_RESULT);
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        app->lock_alert_active = true;
        app->lock_alert_until = app->now + PK_LOCK_ALERT_DISPLAY_MS;
        app->awake = true;
        app->last_activity = app->now;
    } else {
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        app->lock_alert_active = false;
        app->last_activity = app->now;
    }
    pk_app_set_msg_text(app, lock_msg, composed);
    /* VCSEC verify does not create P/speed qualification or a reverse-unlock snapshot. */
    return true;
}
