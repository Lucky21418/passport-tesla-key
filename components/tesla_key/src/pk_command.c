#include "pk_app_internal.h"
#include "pk_plan.h"
#include "pk_risk.h"

#include <string.h>

/*
 * M5: command gate and frozen gesture. The action, vehicle, and
 * qualification generation are locked at key-down. A later refresh must
 * not emit the old gesture; a result must not turn an old LOCK press into
 * UNLOCK. No queue, no default toggle, no 0-car vehicle commands, no
 * commands from the read-only status page.
 *
 * CLIMATE_ON on a driving home long-● is the locked mapping and is not
 * quietly folded into the park-qualification gate (needs a new user decision).
 */

pk_ms_t pk_app_phase_deadline(const pk_app_t *app, pk_ms_t now, pk_ms_t budget)
{
    pk_ms_t phase = now + budget;

    if (!app || app->intent_deadline == 0) {
        return phase;
    }
    if (pk_time_due(now, app->intent_deadline)) {
        return now;
    }
    if (pk_time_before(app->intent_deadline, phase)) {
        return app->intent_deadline;
    }
    return phase;
}

static pk_domain_t action_domain(pk_action_t action)
{
    switch (action) {
    case PK_ACTION_LOCK:
    case PK_ACTION_UNLOCK:
    case PK_ACTION_FRUNK_OPEN:
    case PK_ACTION_TRUNK_MOVE:
    case PK_ACTION_VCSEC_WAKE_ONCE:
    case PK_ACTION_VCSEC_GET_STATUS_ONCE:
    case PK_ACTION_ADDKEY:
        return PK_DOMAIN_VCSEC;
    default:
        return PK_DOMAIN_INFOTAINMENT;
    }
}

static pk_cap_t cap_for_action(const pk_caps_t *c, pk_action_t a)
{
    if (!c || (unsigned)a >= 64) {
        return PK_CAP_UNKNOWN;
    }
    return c->action[(unsigned)a];
}

const char *pk_app_guard(pk_app_t *app, pk_action_t action, const pk_intent_t *params)
{
    pk_msg_id_t why;
    (void)params;
    if (!app) {
        return pk_copy(PK_MSG_NO_TARGET);
    }
    if (app->vehicle == PK_VEHICLE_NONE) {
        why = PK_MSG_NO_TARGET;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (!app->connected) {
        why = PK_MSG_RECONNECT_FIRST;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (app->busy) {
        why = PK_MSG_BUSY_NO_QUEUE;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (!app->paired) {
        why = PK_MSG_NOT_PAIRED;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    /* The same risk-derived table drives the UI availability text. */
    if (pk_action_moving_blocked(action) && pk_app_known_motion(app)) {
        why = PK_MSG_MOVING;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (pk_action_is_park_gated(action) && !pk_app_park_fresh(app)) {
        why = PK_MSG_UNQUALIFIED;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (action != PK_ACTION_LOCK && action != PK_ACTION_UNLOCK &&
        action != PK_ACTION_FRUNK_OPEN && action != PK_ACTION_TRUNK_MOVE &&
        action != PK_ACTION_FLASH && action != PK_ACTION_HONK &&
        action != PK_ACTION_GET_QUALIFICATION &&
        cap_for_action(&app->caps, action) == PK_CAP_NO) {
        why = PK_MSG_CAPABILITY;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (action == PK_ACTION_UNLOCK && !pk_app_can_vcsec_unlock(app)) {
        why = PK_MSG_LOCK_SNAP_BLOCK;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if (action == PK_ACTION_CHARGE_PORT_CLOSE && app->caps.cable_connected == PK_CAP_YES) {
        why = PK_MSG_CABLE;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if ((action == PK_ACTION_CHARGE_PORT_OPEN || action == PK_ACTION_CHARGE_PORT_CLOSE) &&
        app->caps.car_wash == PK_CAP_YES) {
        why = PK_MSG_CAR_WASH;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    if ((action == PK_ACTION_SET_SEAT_HEAT || action == PK_ACTION_SET_STEERING_HEAT) &&
        app->caps.climate_on == PK_CAP_NO && app->caps.keeper_on == PK_CAP_NO) {
        why = PK_MSG_NEED_CLIMATE;
        pk_app_set_msg(app, why);
        return pk_copy(why);
    }
    return NULL;
}

static bool dispatch_select(pk_app_t *app)
{
    pk_scope_t cur = pk_app_scope(app);
    if (app->page == PK_PAGE_STATUS) {
        if (app->connected && !app->has_pending) {
            pk_app_open_status(app, app->origin);
        } else {
            pk_app_exit_status(app);
        }
        return true;
    }
    if (app->page == PK_PAGE_QUALIFICATION_OFFER) {
        if (app->offer_focus == PK_OFFER_CHECK || app->offer_focus == PK_OFFER_WAKE) {
            return pk_app_accept_qualification_offer(app);
        }
        pk_app_decline_qualification_offer(app);
        return true;
    }
    if (app->page == PK_PAGE_SETTINGS) {
        if (app->edit_open) {
            return pk_app_editor_select(app);
        }
        return pk_app_activate_setting(app, app->focus);
    }
    if (app->page == PK_PAGE_RESET_CONFIRM) {
        return pk_app_confirm_reset(app, app->reset_focus == 1);
    }
    if (app->page == PK_PAGE_CARS) {
        return pk_app_select_list_focus(app);
    }
    if (app->page == PK_PAGE_DELETE_CONFIRM) {
        if (app->delete_focus == 1) {
            return pk_app_delete_vehicle(app, app->delete_target, false, PK_STORE_CRASH_NONE);
        }
        (void)pk_app_navigate(app, PK_PAGE_CARS);
        return true;
    }
    if (app->page == PK_PAGE_LOGS) {
        (void)pk_app_navigate(app, PK_PAGE_SETTINGS);
        return true;
    }
    if (app->page == PK_PAGE_PAIR) {
        return pk_app_pair_next(app);
    }
    if (app->page == PK_PAGE_VIN) {
        return true;
    }
    (void)cur;
    return true;
}

static void close_send(pk_app_t *app, pk_msg_id_t msg)
{
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    app->has_deferred = false;
    app->intent_deadline = 0;
    pk_app_set_busy(app, false);
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    pk_app_set_msg(app, msg);
}

/* Map a real transport result onto the pending command. PK_OK with
 * DELIVERED is "handed to GATT, waiting for the vehicle", not vehicle
 * success. PK_OK without delivery is queued and must not say sent.
 * Any other result closes the intent. */
static bool finish_command_send(pk_app_t *app, pk_err_t rc, pk_action_t action)
{
    pk_tx_outcome_t outcome = pk_app_note_send(app, rc);

    if (rc == PK_OK && outcome == PK_TX_DELIVERED) {
        pk_app_set_msg(app, PK_MSG_SENT_WAIT);
        return true;
    }
    if (rc == PK_OK) {
        return true;
    }
    if (outcome == PK_TX_SENT_UNKNOWN) {
        close_send(app, (action == PK_ACTION_LOCK) ? PK_MSG_LOCK_UNCONFIRMED
                                                    : PK_MSG_CMD_UNCONFIRMED);
        if (action == PK_ACTION_LOCK) {
            app->lock_fsm = PK_LOCK_FSM_RESULT;
        }
        return true;
    }
    close_send(app, PK_MSG_CANCELLED);
    return false;
}

bool pk_app_commit(pk_app_t *app, const pk_intent_t *intent, pk_ms_t t)
{
    const char *why;
    pk_scope_t cur;
    pk_pending_t *p;
    pk_intent_t active;

    if (!app || !intent) {
        return false;
    }
    if (intent->action == PK_ACTION_RETURN_MAIN || intent->action == PK_ACTION_SELECT) {
        cur = pk_app_scope(app);
        if (!pk_scope_full_match(&intent->scope, &cur)) {
            return false;
        }
        if (intent->action == PK_ACTION_RETURN_MAIN) {
            return pk_app_navigate(app, PK_PAGE_MAIN);
        }
        return dispatch_select(app);
    }
    if (intent->action == PK_ACTION_RECONNECT) {
        return pk_app_start_reconnect(app, true);
    }
    if (app->page == PK_PAGE_STATUS) {
        pk_app_set_msg(app, PK_MSG_STATUS_READONLY);
        return false;
    }
    cur = pk_app_scope(app);
    if (!pk_scope_full_match(&intent->scope, &cur) || intent->target != app->vehicle) {
        pk_app_set_msg(app, PK_MSG_OLD_GESTURE);
        return false;
    }
    /* A rejected overlap is not a cancellation. Leave the frozen gesture,
     * its parameters, and its wake budget untouched. */
    if (app->busy) {
        pk_app_set_msg(app, PK_MSG_BUSY_NO_QUEUE);
        return false;
    }
    {
        bool continuation = app->has_deferred && intent->action == app->deferred.action;
        bool asleep_info = app->vcsec_status_valid && app->vcsec_asleep &&
                           action_domain(intent->action) == PK_DOMAIN_INFOTAINMENT;
        bool need_qual = !pk_app_known_motion(app) &&
                         pk_action_is_park_gated(intent->action) &&
                         !pk_app_park_fresh(app);

        active = *intent;
        if (continuation) {
            if (app->intent_deadline != 0 && pk_time_due(t, app->intent_deadline)) {
                app->has_deferred = false;
                app->intent_deadline = 0;
                pk_app_set_msg(app, PK_MSG_UNQUALIFIED);
                return false;
            }
            active = app->deferred;
            active.scope = pk_app_scope(app);
            app->has_deferred = false;
        } else {
            const char *blocked = pk_app_guard(app, active.action, &active);
            /* Missing park is acceptance into one check-before-act read.
             * Every other guard failure is a rejection: keep the previous
             * intent, frozen parameters, wake owner, spent bit, and deadline. */
            if (blocked && !(app->message == PK_MSG_UNQUALIFIED && need_qual)) {
                pk_arbiter_invalidate(&app->keys);
                return false;
            }
            pk_ms_t budget = pk_plan_deadline(PK_PLAN_COMMAND);
            app->wake_owner = PK_WAKE_OWNER_COMMAND;
            app->wake_spent = false;
            app->has_deferred = false;
            if (asleep_info) {
                budget = pk_plan_deadline(PK_PLAN_WAKE_THEN_READ);
            } else if (need_qual) {
                budget = pk_plan_deadline(PK_PLAN_QUALIFY);
            }
            /* The first phase budget is the whole intent. Later phases clip to it. */
            app->intent_deadline = t + budget;
        }
        intent = &active;
    }
    /* Car known asleep: one wake inside this intent, then the first submit.
     * A spent budget or a failed wake does not invent another wake or intent. */
    if (!app->wake_spent && !app->has_deferred && app->connected &&
        app->paired && app->vcsec_status_valid && app->vcsec_asleep &&
        action_domain(intent->action) == PK_DOMAIN_INFOTAINMENT) {
        pk_intent_t frozen = *intent;
        if (!pk_app_start_wake_then(app)) {
            pk_app_set_msg(app, PK_MSG_WAKE_FAIL);
            return false;
        }
        app->deferred = frozen;
        app->has_deferred = true;
        return true;
    }
    /* Check-before-act: a park-gated gesture is frozen here and a single
     * drive-state read decides it. The user never has to perform a separate
     * "qualification" step, and a stale grant cannot silently block a command
     * that the car would allow. */
    if (!app->has_deferred && app->connected && app->paired &&
        !pk_app_known_motion(app) &&
        pk_action_is_park_gated(intent->action) && !pk_app_park_fresh(app)) {
        pk_intent_t frozen = *intent;
        if (pk_app_request_qualification(app, false, false)) {
            app->deferred = frozen;
            app->has_deferred = true;
            return true;
        }
    }
    why = pk_app_guard(app, intent->action, intent);
    if (why) {
        pk_arbiter_invalidate(&app->keys);
        return false;
    }

    /* A background status probe may still own the one-shot wire slot. Cancel it
     * before a user gesture; never overwrite an active transaction. */
    pk_ble_release_in_flight(app);
    app->serial++;
    pk_arbiter_invalidate(&app->keys);
    app->edit_open = false;
    app->editing = false;
    pk_app_set_busy(app, true);

    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_COMMAND;
    p->action = intent->action;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    p->deadline = pk_app_phase_deadline(app, t, pk_plan_deadline(PK_PLAN_COMMAND));
    app->has_pending = true;
    app->lock_fsm = (intent->action == PK_ACTION_LOCK) ? PK_LOCK_FSM_LOCK_PENDING
                                                       : PK_LOCK_FSM_IDLE;
    p->bool_enabled = intent->bool_enabled;
    {
        pk_err_t rc = pk_ble_send_once(app, action_domain(intent->action), intent->action,
                                       p->deadline);
        pk_app_log(app, PK_LOG_EVENT_COMMAND_INTENT, PK_LOG_ERR_NONE);
        return finish_command_send(app, rc, intent->action);
    }
}

bool pk_app_command_result(pk_app_t *app, pk_id_t op_id, pk_cmd_result_t result, bool authenticated)
{
    pk_action_t action;
    pk_scope_t cur;

    if (!app || authenticated != true || !app->has_pending ||
        app->pending.kind != PK_PENDING_COMMAND ||
        app->pending.id != op_id || pk_time_due(app->now, app->pending.deadline)) {
        return false;
    }
    /* Terminal results must match the frozen vehicle + connection +
     * session. Generation is not part of this check: an in-flight
     * result must not die only because a grant expired.
     */
    cur = pk_app_scope(app);
    if (!pk_scope_same_vehicle_conn_session(&app->pending.scope, &cur)) {
        return false;
    }
    if (result == PK_CMD_RESULT_WAIT) {
        /* Intermediate. Does not extend the deadline. Not a terminal accept. */
        pk_app_set_msg(app, PK_MSG_CMD_WAIT_MID);
        return false;
    }
    if (result != PK_CMD_RESULT_ACCEPTED && result != PK_CMD_RESULT_REJECTED &&
        result != PK_CMD_RESULT_UNCONFIRMED) {
        pk_app_set_msg(app, PK_MSG_CMD_UNRECOGNIZED);
        return false;
    }
    if (app->pending.action == PK_ACTION_LOCK) {
        return pk_app_start_lock_verification(app, result);
    }
    if (app->pending.action == PK_ACTION_UNLOCK && result == PK_CMD_RESULT_ACCEPTED &&
        app->connected && app->vehicle != PK_VEHICLE_NONE) {
        pk_ble_release_in_flight(app);
        app->serial++;
        memset(&app->pending, 0, sizeof(app->pending));
        app->pending.kind = PK_PENDING_UNLOCK_VERIFY;
        app->pending.action = PK_ACTION_VCSEC_GET_STATUS_ONCE;
        app->pending.id = app->serial;
        app->pending.scope = pk_app_scope(app);
        /* N08: this read-only VCSEC window is 3000 ms from verification
         * start. It is not clipped to the command total, and it does not
         * wake, retransmit, or reconnect. */
        app->pending.deadline = app->now + pk_plan_deadline(PK_PLAN_UNLOCK_VERIFY);
        app->intent_deadline = 0;
        app->has_pending = true;
        {
            pk_err_t rc = pk_ble_send_once(app, PK_DOMAIN_VCSEC,
                                           PK_ACTION_VCSEC_GET_STATUS_ONCE,
                                           app->pending.deadline);
            pk_tx_outcome_t outcome = pk_app_note_send(app, rc);
            if (rc != PK_OK) {
                close_send(app, (outcome == PK_TX_SENT_UNKNOWN) ? PK_MSG_CMD_UNCONFIRMED
                                                                 : PK_MSG_CANCELLED);
                return false;
            }
        }
        return true;
    }
    action = app->pending.action;
    (void)action;
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false);
    if (result == PK_CMD_RESULT_ACCEPTED) {
        /* ACCEPTED only means the vehicle accepted the command, NOT that the
         * actuator has already reached the target state. Reported caps come
         * strictly from authenticated vehicle observations. */
        pk_app_set_msg(app, PK_MSG_CMD_ACCEPTED);
    } else if (result == PK_CMD_RESULT_REJECTED) {
        pk_app_set_msg(app, PK_MSG_CMD_REJECTED);
    } else {
        pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
    }
    app->intent_deadline = 0;
    if (app->low_battery) {
        app->last_activity = app->now;
    }
    return true;
}

bool pk_app_command_legacy_receipt(pk_app_t *app, pk_id_t op_id, pk_cmd_result_t result)
{
    pk_action_t action;
    pk_scope_t cur;

    if (!app || !app->has_pending ||
        app->pending.kind != PK_PENDING_COMMAND ||
        app->pending.id != op_id || pk_time_due(app->now, app->pending.deadline)) {
        return false;
    }
    cur = pk_app_scope(app);
    if (!pk_scope_same_vehicle_conn_session(&app->pending.scope, &cur)) {
        return false;
    }
    if (result == PK_CMD_RESULT_WAIT) {
        pk_app_set_msg(app, PK_MSG_CMD_WAIT_MID);
        return false;
    }
    if (result != PK_CMD_RESULT_ACCEPTED && result != PK_CMD_RESULT_REJECTED &&
        result != PK_CMD_RESULT_UNCONFIRMED) {
        pk_app_set_msg(app, PK_MSG_CMD_UNRECOGNIZED);
        return false;
    }
    action = app->pending.action;
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false);
    /* S2 §1: Weak legacy receipt MUST NOT claim vehicle execution
     * (PK_MSG_CMD_ACCEPTED), MUST NOT claim authenticated rejection, MUST NOT
     * mutate optimistic app->caps.*, and MUST NOT confirm lock/unlock.
     * Surface existing honest unconfirmed status without adding UI pages. */
    if (action == PK_ACTION_LOCK) {
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        pk_app_set_msg(app, PK_MSG_LOCK_UNCONFIRMED);
    } else {
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
    }
    app->intent_deadline = 0;
    if (app->low_battery) {
        app->last_activity = app->now;
    }
    return true;
}

void pk_app_exit_wait(pk_app_t *app)
{
    bool was_result;
    if (!app) {
        return;
    }
    was_result = (app->page == PK_PAGE_RESULT && !app->has_pending);
    if (app->has_pending && app->pending.kind == PK_PENDING_QUALIFICATION && app->has_recheck) {
        app->has_recheck = false;
        app->edit_open = false;
        memset(&app->draft, 0, sizeof(app->draft));
    }
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    app->lock_alert_active = false;
    app->result_auto_return = false;
    app->has_deferred = false;
    app->intent_deadline = 0;
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false);
    if (app->origin == PK_ORIGIN_DRIVING) {
        app->origin = PK_ORIGIN_LIST;
        app->page = PK_PAGE_MAIN;
    } else {
        app->page = app->modal_return_page;
    }
    pk_app_set_msg(app, was_result ? PK_MSG_EMPTY : PK_MSG_WAIT_EXIT);
}
