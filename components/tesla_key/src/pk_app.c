#include "pk_app_internal.h"
#include "pk_power.h"

#include <string.h>

/*
 * Application-layer orchestration.
 *
 * Same-timestamp order (C04 / M1):
 *   1. connection / state invalidation
 *   2. key edge (a second PRESS at t <= deadline still wins)
 *   3. committable timers (a single click only when t > deadline)
 *
 * tick(deadline) never commits a click. Committing a due click immediately
 * before a simultaneous PRESS at t > deadline keeps timer-first and
 * down-first equivalent: the click is not swallowed by a late new press,
 * and a press exactly at the deadline is still a double-click.
 *
 * This module is host-testable. The ESP-IDF build wires pk_hal_t through
 * main/; do not treat host-only tests as a vehicle result.
 */

static void expire_pending(pk_app_t *app)
{
    pk_pending_kind_t kind;

    if (!app->has_pending || pk_time_before(app->now, app->pending.deadline)) {
        return;
    }
    kind = app->pending.kind;
    if (app->pending.action == PK_ACTION_LOCK) {
        if (app->connected) {
            (void)pk_app_start_lock_verification(app, PK_CMD_RESULT_TIMEOUT);
        } else {
            pk_tx_outcome_t outcome = app->last_tx_outcome;
            pk_ble_release_in_flight(app);
            app->has_pending = false;
            pk_app_set_busy(app, false);
            pk_arbiter_invalidate(&app->keys);
            app->lock_fsm = PK_LOCK_FSM_RESULT;
            if (app->vehicle != PK_VEHICLE_NONE && !app->ap &&
                app->page != PK_PAGE_PAIR && !app->pair_radio_held && !app->low_battery) {
                (void)pk_app_start_reconnect(app, false);
            }
            pk_app_set_msg(app, (outcome == PK_TX_NOT_SENT) ? PK_MSG_CANCELLED
                                                            : PK_MSG_LOCK_UNCONFIRMED);
        }
        return;
    }
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    app->status_need_send = false;
    pk_app_set_busy(app, false);
    pk_arbiter_invalidate(&app->keys);
    if (kind == PK_PENDING_QUALIFICATION) {
        app->has_deferred = false;
        app->intent_deadline = 0;
        pk_app_set_msg(app, PK_MSG_UNQUALIFIED);
    } else if (kind == PK_PENDING_STATUS) {
        app->has_deferred = false;
        if (app->wake_owner != PK_WAKE_OWNER_STATUS ||
            app->intent_deadline == 0 ||
            pk_time_due(app->now, app->intent_deadline)) {
            app->intent_deadline = 0;
        }
        pk_app_set_msg(app, PK_MSG_STATUS_TIMEOUT);
    } else if (kind == PK_PENDING_LOCK_VERIFY) {
        app->has_deferred = false;
        app->intent_deadline = 0;
        app->vcsec_status_valid = false;
        app->vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
        pk_app_set_msg(app, PK_MSG_LOCK_UNCONFIRMED);
        pk_app_present_modal(app, PK_PAGE_RESULT);
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        app->lock_alert_active = true;
        app->lock_alert_until = app->now + PK_LOCK_ALERT_DISPLAY_MS;
        app->awake = true;
        app->last_activity = app->now;
    } else {
        app->has_deferred = false;
        app->intent_deadline = 0;
        if (kind == PK_PENDING_WAKE_THEN) {
            pk_app_set_msg(app, PK_MSG_WAKE_TIMEOUT);
        } else if (kind == PK_PENDING_UNLOCK_VERIFY) {
            /* Do not keep the pre-unlock LOCKED snapshot: that leaves the primary
             * action as UNLOCK and makes the next lock press send UNLOCK again. */
            app->vcsec_status_valid = false;
            app->vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
            pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
        } else {
            pk_app_set_msg(app, PK_MSG_CMD_TIMEOUT);
        }
    }
    if (kind == PK_PENDING_STATUS) {
        /* Keep only fields that were present; do not invent defaults. */
        unsigned i, n = 0;
        for (i = 0; i < app->field_count; i++) {
            if (app->fields[i].present) {
                if (n != i) {
                    app->fields[n] = app->fields[i];
                }
                n++;
            }
        }
        app->field_count = n;
        strncpy(app->status_phase, n ? "partial" : "timeout", sizeof(app->status_phase) - 1);
    }
}

void pk_app_expire_state(pk_app_t *app, pk_ms_t t)
{
    if (!app) {
        return;
    }
    app->now = t;
    if (app->motion == PK_MOTION_TENDENCY && app->motion_until != 0 &&
        pk_time_due(t, app->motion_until)) {
        pk_app_motion_unknown(app);
    }
    if (app->has_pending && app->pending.kind == PK_PENDING_WAKE_THEN) {
        if (pk_time_due(t, app->pending.deadline)) {
            if (app->pending.qphase == 1) {
                bool status = app->wake_then_status;
                bool deferred = app->has_deferred;
                pk_intent_t frozen = app->deferred;
                pk_ble_release_in_flight(app);
                app->has_pending = false;
                pk_app_set_busy(app, false);
                app->wake_then_status = false;
                /* Do not clear vcsec_asleep. The spent wake budget, not a
                 * fabricated awake flag, stops a second wake. */
                if (status) {
                    if (app->intent_deadline != 0 &&
                        pk_time_due(t, app->intent_deadline)) {
                        app->intent_deadline = 0;
                        strncpy(app->status_phase, "timeout",
                                sizeof(app->status_phase) - 1);
                        pk_app_set_msg(app, PK_MSG_STATUS_TIMEOUT);
                    } else {
                        pk_app_open_status(app, app->origin);
                    }
                } else if (deferred) {
                    frozen.scope = pk_app_scope(app);
                    app->deferred = frozen;
                    app->has_deferred = true;
                    (void)pk_app_commit(app, &frozen, t);
                }
                return;
            }
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
    if (app->has_grant && pk_time_due(t, app->grant.until)) {
        pk_grant_clear(&app->grant);
        app->has_grant = false;
        pk_app_bump_generation(app);
    }
    expire_pending(app);
    if (app->lock_alert_active && pk_time_due(t, app->lock_alert_until)) {
        app->lock_alert_active = false;
    }
    if (app->result_auto_return && pk_time_due(t, app->result_return_until)) {
        app->result_auto_return = false;
        if (app->page == PK_PAGE_RESULT) {
            app->lock_fsm = PK_LOCK_FSM_IDLE;
            app->page = (app->modal_return_page == PK_PAGE_SETTINGS) ? PK_PAGE_SETTINGS : PK_PAGE_MAIN;
            pk_app_set_msg(app, PK_MSG_EMPTY);
        }
    }
    if (app->auth_until && pk_time_due(t, app->auth_until)) {
        app->auth = PK_MSG_AUTH_EXPIRED;
        app->auth_until = 0; /* never renew vehicle-side authorization */
        pk_app_set_msg(app, PK_MSG_AUTH_EXPIRED);
    }
    if (app->ap && pk_time_due(t, app->ap_deadline)) {
        pk_app_close_ap(app, "ap-expired");
        pk_app_set_msg(app, PK_MSG_AP_EXPIRED);
    }
    if (app->reconnect.active && pk_time_due(t, app->reconnect.next)) {
        (void)pk_app_reconnect_attempt(app, false);
    }
    if ((app->pair_step == 3 || app->pair_step == 4) &&
        app->pair_deadline && pk_time_due(t, app->pair_deadline)) {
        pk_app_pair_cancel(app);
        pk_app_set_msg(app, PK_MSG_PAIR_TIMEOUT);
        (void)pk_app_navigate(app, PK_PAGE_CARS);
    }
}

static pk_err_t set_now(pk_app_t *app, pk_ms_t now)
{
    if (!app) {
        return PK_ERR_INVALID;
    }
    if (app->time_set && pk_time_before(now, app->now)) {
        return PK_ERR_INVALID; /* monotonic watermark required */
    }
    app->now = now;
    app->time_set = true;
    return PK_OK;
}

static void after_edge_timers(pk_app_t *app, pk_ms_t now)
{
    pk_ms_t screen_budget = pk_power_screen_timeout(app);
    pk_keys_tick(app, now);
    pk_side_tick(app, now);
    if (app->awake && !app->lock_alert_active &&
        pk_time_elapsed(now, app->last_activity, screen_budget)) {
        pk_app_sleep(app);
    }
}

pk_err_t pk_app_tick(pk_app_t *app, pk_ms_t now)
{
    pk_err_t e = set_now(app, now);
    if (e != PK_OK) {
        return e;
    }
    pk_app_expire_state(app, now);
    pk_app_status_pump(app);
    after_edge_timers(app, now);
    return PK_OK;
}

pk_err_t pk_app_button_edge(pk_app_t *app, pk_ms_t now, pk_key_id_t key, pk_edge_t edge)
{
    pk_err_t e;

    if (!app) {
        return PK_ERR_INVALID;
    }
    /* Only raw PRESS / RELEASE are accepted. There is no parameter for
     * BUTTON_SINGLE_CLICK / DOUBLE_CLICK / LONG_PRESS_START. Adapters
     * that would translate BSP 180/500 cooked events into this API are
     * out of contract (M1). An invalid ADC reading must not be turned
     * into a press by the adapter.
     */
    if (key != PK_KEY_UP && key != PK_KEY_DOT && key != PK_KEY_DOWN && key != PK_KEY_POWER) {
        return PK_ERR_INVALID;
    }
    if (edge != PK_EDGE_PRESS && edge != PK_EDGE_RELEASE) {
        return PK_ERR_INVALID;
    }

    e = set_now(app, now);
    if (e != PK_OK) {
        return e;
    }

    /* 1. Connection / state invalidation first. */
    pk_app_expire_state(app, now);

    /* 2 / 3. If a new ● press arrives after the double-click window, commit
     * the pending single click first so it is not swallowed. At t ==
     * deadline, do not commit — the second press still wins.
     */
    if (key == PK_KEY_DOT && edge == PK_EDGE_PRESS) {
        pk_keys_commit_click_if_due(app, now);
        pk_keys_dot_press(app, now);
    } else if (key == PK_KEY_DOT && edge == PK_EDGE_RELEASE) {
        pk_keys_dot_release(app, now);
    } else if ((key == PK_KEY_UP || key == PK_KEY_DOWN) && edge == PK_EDGE_PRESS) {
        (void)pk_side_press(app, key, now);
    } else if ((key == PK_KEY_UP || key == PK_KEY_DOWN) && edge == PK_EDGE_RELEASE) {
        pk_side_release(app, key, now);
    } else if (key == PK_KEY_POWER && edge == PK_EDGE_PRESS) {
        pk_power_press(app, now);
    } else if (key == PK_KEY_POWER && edge == PK_EDGE_RELEASE) {
        pk_power_release(app, now);
    }

    after_edge_timers(app, now);
    return PK_OK;
}
