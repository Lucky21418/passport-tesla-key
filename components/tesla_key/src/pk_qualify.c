#include "pk_app_internal.h"
#include "pk_plan.h"

#include <string.h>

/*
 * M4: qualification lifecycle and a single user-triggered wake.
 *
 * Connection is not P / stationary / locked / drive authorization.
 * First connect, vehicle sleep, or expired grant: explicit LOCK remains
 * available if the basic pairing is valid; dangerous actions stay off.
 *
 * Settings row 1 or a failed-save offer starts one bounded serial
 * DriveState + ClosuresState read, with no automatic wake.
 * After an insufficient read (including a normal timeout) and no confirmed
 * motion, present the existing offer focused on "唤醒车辆并检查" / "返回".
 * Only the user's ● on "唤醒车辆并检查" may issue one
 * VCSEC RKE_ACTION_WAKE_VEHICLE, then wait for a valid reply, then retry
 * the read. "返回" does not wake and does not replay an old command.
 *
 * request_qualification(wake=true) is rejected unless the caller holds
 * the user-gesture token from that offer. A direct internal call cannot
 * stand in for the path (C01).
 *
 * Post-LOCK VCSEC verification must never call this wake path (M6).
 *
 * P requires a verified enum P, at least one speed field truly present,
 * and every present speed field consistently 0. D/R/N, non-zero, NaN,
 * conflicts, missing, Invalid/SNA do not qualify. Lock snapshot is
 * independent (ClosuresState.optional_locked + timestamp).
 *
 * clock_verified and adapter_verified default false. Without a verified
 * timebase, dangerous actions stay blocked (production gate, 待真机).
 * Do not treat a public Wakeup / GetState as proof of this vehicle's
 * timestamp or response capability. Do not use a synthetic epoch as a
 * real vehicle clock.
 */

void pk_app_present_wake_offer(pk_app_t *app, pk_msg_id_t why)
{
    if (!app) {
        return;
    }
    app->wake_choice_available = true;
    app->offer_focus = PK_OFFER_WAKE;
    app->page = PK_PAGE_QUALIFICATION_OFFER;
    pk_app_set_busy(app, false);
    pk_app_set_msg(app, why);
}

bool pk_app_start_wake_then(pk_app_t *app)
{
    pk_pending_t *p;
    pk_err_t rc;

    if (!app || app->busy || app->has_pending || app->vehicle == PK_VEHICLE_NONE ||
        !app->connected || !app->paired || app->wake_spent ||
        app->lock_fsm == PK_LOCK_FSM_VERIFY) {
        return false;
    }
    /* Spend the budget before the send so a failure cannot loop. */
    app->wake_spent = true;
    app->serial++;
    pk_app_invalidate(app, PK_MSG_QUAL_IN_PROGRESS);
    pk_app_set_busy(app, true);
    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_WAKE_THEN;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    p->action = PK_ACTION_VCSEC_WAKE_ONCE;
    p->qphase = 0; /* 0 = wake wait, 1 = settle */
    p->deadline = pk_app_phase_deadline(app, app->now,
                                       pk_plan_deadline(PK_PLAN_WAKE_THEN_READ));
    app->has_pending = true;
    rc = pk_ble_send_once(app, PK_DOMAIN_VCSEC, PK_ACTION_VCSEC_WAKE_ONCE, p->deadline);
    if (rc != PK_OK) {
        pk_tx_outcome_t outcome = pk_app_note_send(app, rc);
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
        pk_app_set_msg(app, (outcome == PK_TX_SENT_UNKNOWN) ? PK_MSG_WAKE_FAIL
                                                            : PK_MSG_CANCELLED);
        return false;
    }
    (void)pk_app_note_send(app, rc);
    return true;
}

bool pk_app_request_qualification(pk_app_t *app, bool wake, bool user_selected_wake)
{
    pk_ms_t budget;
    pk_pending_t *p;

    if (!app) {
        return false;
    }
    if (wake && !user_selected_wake) {
        /* Refuse a wake that did not come from the offer focus. */
        return false;
    }
    if (app->vehicle == PK_VEHICLE_NONE || !app->connected || app->busy) {
        pk_app_set_msg(app, PK_MSG_QUAL_NEED);
        pk_arbiter_invalidate(&app->keys);
        return false;
    }

    pk_app_invalidate(app, PK_MSG_QUAL_IN_PROGRESS);
    app->serial++;
    pk_app_set_busy(app, true);
    budget = wake ? pk_plan_deadline(PK_PLAN_WAKE_THEN_READ)
                     : pk_plan_deadline(PK_PLAN_QUALIFY);

    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_QUALIFICATION;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    p->requested = app->now;
    p->deadline = pk_app_phase_deadline(app, app->now, budget);
    p->wake = wake;
    p->qphase = wake ? PK_QPHASE_WAKE_WAIT : PK_QPHASE_READ_WAIT;
    p->action = wake ? PK_ACTION_VCSEC_WAKE_ONCE : PK_ACTION_GET_QUALIFICATION;
    app->has_pending = true;

    if (wake) {
        if (app->wake_owner != PK_WAKE_OWNER_QUALIFY) {
            app->wake_owner = PK_WAKE_OWNER_QUALIFY;
            app->wake_spent = false;
        }
        if (app->wake_spent) {
            pk_ble_release_in_flight(app);
            app->has_pending = false;
            pk_app_set_busy(app, false);
            pk_app_set_msg(app, PK_MSG_WAKE_FAIL);
            return false;
        }
        app->wake_spent = true;
    }
    {
        pk_err_t rc = pk_ble_send_once(app,
                                       wake ? PK_DOMAIN_VCSEC : PK_DOMAIN_INFOTAINMENT,
                                       p->action, p->deadline);
        pk_tx_outcome_t outcome = pk_app_note_send(app, rc);
        if (rc != PK_OK) {
            pk_ble_release_in_flight(app);
            app->has_pending = false;
            pk_app_set_busy(app, false);
            pk_app_set_msg(app, (outcome == PK_TX_SENT_UNKNOWN) ? PK_MSG_CMD_UNCONFIRMED
                                                                : PK_MSG_CANCELLED);
            return false;
        }
    }
    pk_app_log(app, PK_LOG_EVENT_QUERY, PK_LOG_ERR_NONE);
    return true;
}

bool pk_app_accept_qualification_offer(pk_app_t *app)
{
    if (!app) {
        return false;
    }
    if (app->offer_focus == PK_OFFER_WAKE) {
        return pk_app_request_qualification(app, true, true);
    }
    return pk_app_request_qualification(app, false, false);
}

void pk_app_decline_qualification_offer(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->has_deferred = false;
    pk_app_set_msg(app, PK_MSG_WAKE_DECLINED);
    if (app->has_recheck || app->edit_open) {
        pk_app_cancel_editor(app);
    } else {
        pk_arbiter_invalidate(&app->keys);
        if (app->modal_return_page == PK_PAGE_SETTINGS || app->modal_return_page == PK_PAGE_MAIN ||
            app->modal_return_page == PK_PAGE_CARS) {
            app->page = app->modal_return_page;
        } else {
            app->page = PK_PAGE_SETTINGS;
        }
    }
}

bool pk_app_qualification_wake_result(pk_app_t *app, pk_id_t request_id, const pk_scope_t *scope,
                                      bool accepted, bool authenticated)
{
    if (!app || !app->has_pending || !scope) {
        return false;
    }
    {
        pk_scope_t cur = pk_app_scope(app);
        if (app->pending.id != request_id ||
            app->pending.qphase != PK_QPHASE_WAKE_WAIT ||
            pk_time_due(app->now, app->pending.deadline) ||
            !pk_scope_full_match(&app->pending.scope, &cur) ||
            !pk_scope_full_match(scope, &app->pending.scope)) {
            return false;
        }
    }
    if (accepted != true || authenticated != true) {
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        pk_app_present_modal(app, PK_PAGE_RESULT);
        pk_app_set_msg(app, PK_MSG_WAKE_FAIL);
        return false;
    }
    app->pending.qphase = PK_QPHASE_READ_WAIT;
    app->pending.action = PK_ACTION_GET_QUALIFICATION;
    pk_ble_release_in_flight(app);
    (void)pk_ble_send_once(app, PK_DOMAIN_INFOTAINMENT, PK_ACTION_GET_QUALIFICATION,
                           app->pending.deadline);
    return true;
}

bool pk_app_qualification_wake_legacy_ack(pk_app_t *app, pk_id_t request_id,
                                          const pk_scope_t *scope)
{
    pk_scope_t cur;
    if (!app || !app->has_pending || !scope) {
        return false;
    }
    cur = pk_app_scope(app);
    if (app->pending.kind != PK_PENDING_QUALIFICATION ||
        app->pending.id != request_id ||
        app->pending.qphase != PK_QPHASE_WAKE_WAIT ||
        pk_time_due(app->now, app->pending.deadline) ||
        !pk_scope_full_match(&app->pending.scope, &cur) ||
        !pk_scope_full_match(scope, &app->pending.scope)) {
        return false;
    }
    /* S2 §1: Under an already-authorized bounded wake intent, an attributed
     * legacy wake ACK may advance once into the read-only qualification query
     * within the existing deadline (no extra wake budget, no write intent). */
    app->pending.qphase = PK_QPHASE_READ_WAIT;
    app->pending.action = PK_ACTION_GET_QUALIFICATION;
    pk_ble_release_in_flight(app);
    (void)pk_ble_send_once(app, PK_DOMAIN_INFOTAINMENT, PK_ACTION_GET_QUALIFICATION,
                           app->pending.deadline);
    return true;
}

/*
 * A grant is evidence about the state at *read time*, so its lease starts when
 * the reply arrived. VehicleData timestamps describe the last change of a
 * section, not the freshness of the answer: clamping the lease to
 * sample + lease made every long-parked car unqualified the moment it was
 * read (sample older than now, until already in the past).
 */
static pk_ms_t grant_lease_until(pk_ms_t now, pk_ms_t lease_ms)
{
    return now + lease_ms;
}

static void apply_qualify_recheck(pk_app_t *app)
{
    pk_intent_t intent;

    if (!app->has_recheck ||
        app->recheck.vehicle != app->vehicle ||
        app->recheck.connection != app->connection ||
        app->recheck.session != app->session) {
        return;
    }
    if (app->recheck.key == PK_SETTING_SENTRY) {
        /* Direct toggle: qualification only gated the command, not a draft. */
        app->has_recheck = false;
        app->edit_open = false;
        intent = pk_app_make_intent(app, PK_ACTION_SET_SENTRY);
        intent.bool_enabled = app->recheck.bool_enabled;
        (void)pk_app_commit(app, &intent, app->now);
        return;
    }
    if (app->recheck.key == PK_SETTING_PORT) {
        pk_action_t action;
        app->has_recheck = false;
        app->edit_open = false;
        if (app->caps.port_open == PK_CAP_YES) {
            action = PK_ACTION_CHARGE_PORT_CLOSE;
        } else if (app->caps.port_open == PK_CAP_NO) {
            action = PK_ACTION_CHARGE_PORT_OPEN;
        } else {
            pk_app_set_msg(app, PK_MSG_STATUS_TIMEOUT);
            return;
        }
        intent = pk_app_make_intent(app, action);
        (void)pk_app_commit(app, &intent, app->now);
        return;
    }
    pk_app_begin_editor(app, app->recheck.key);
    app->draft = app->recheck;
    app->modal_return_page = PK_PAGE_SETTINGS;
    app->has_recheck = false;
}

static void finish_qualify_outcome(pk_app_t *app, bool parked)
{
    if (parked) {
        apply_qualify_recheck(app);
        app->wake_choice_available = false;
        if (app->has_deferred) {
            pk_intent_t frozen = app->deferred;
            pk_scope_t cur = pk_app_scope(app);
            /* Replay the exact gesture the user froze; the grant is fresh.
             * The generation bump from the check itself is an internal epoch,
             * not a new gesture: keep the frozen vehicle/connection/session
             * and adopt the current generation. has_deferred stays set so
             * commit treats this as the same intent. */
            if (frozen.target == app->vehicle &&
                frozen.scope.vehicle == cur.vehicle &&
                frozen.scope.connection == cur.connection &&
                frozen.scope.session == cur.session) {
                frozen.scope = cur;
                app->deferred = frozen;
                app->has_deferred = true;
                (void)pk_app_commit(app, &frozen, app->now);
            } else {
                app->has_deferred = false;
            }
        }
        return;
    }
    app->has_deferred = false;
    app->wake_choice_available = false;
    /* The user asked for an action, not for a gear lecture. Fail closed with a
     * generic result and keep the underlying check out of the UI. */
    pk_app_set_msg(app, app->moving ? PK_MSG_MOVING : PK_MSG_UNQUALIFIED);
}

void pk_qualify_fill_drive(pk_qualify_sample_t *qs, pk_id_t request_id,
                           const pk_scope_t *scope, bool has_drive, pk_shift_t shift,
                           bool speed_present, int speed,
                           bool speed_float_present, int speed_float,
                           bool speed_invalid, bool locked_present, bool locked,
                           bool crypto_auth, int64_t now)
{
    if (!qs) {
        return;
    }
    memset(qs, 0, sizeof(*qs));
    qs->request_id = request_id;
    if (scope) {
        qs->scope = *scope;
    }
    qs->shift = has_drive ? shift : PK_SHIFT_ABSENT;
    qs->speed_present = has_drive && speed_present;
    qs->speed = speed;
    qs->speed_float_present = has_drive && speed_float_present;
    qs->speed_float = speed_float;
    qs->speed_invalid = has_drive && speed_invalid;
    qs->locked_present = locked_present;
    qs->locked = locked;
    /* Illegal speed stays flagged. The drive snapshot itself remains so a
     * trusted D/R/N or a finite non-zero sibling can still be motion. */
    qs->sample_present = has_drive && crypto_auth;
    qs->sample = now;
    qs->lock_sample_present = locked_present && crypto_auth;
    qs->lock_sample = now;
    qs->clock_verified = crypto_auth;
    qs->adapter_verified = crypto_auth;
}

bool pk_app_qualification_result(pk_app_t *app, const pk_qualify_sample_t *s)
{
    bool stopped;
    bool proof;
    bool parked;
    bool lock_proof;
    bool moving;
    int nspeed = 0;
    pk_ms_t requested_at = 0;
    pk_scope_t cur;

    if (!app || !app->has_pending || !s) {
        return false;
    }
    cur = pk_app_scope(app);
    if (app->pending.kind != PK_PENDING_QUALIFICATION ||
        app->pending.qphase != PK_QPHASE_READ_WAIT ||
        app->pending.id != s->request_id ||
        !pk_scope_full_match(&app->pending.scope, &cur) ||
        !pk_scope_full_match(&s->scope, &app->pending.scope) ||
        pk_time_due(app->now, app->pending.deadline)) {
        return false;
    }
    requested_at = app->pending.requested;

    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false); /* generation change invalidates every old gesture */

    /* A missing optional speed is not zero. A negative value is the
     * decoder's illegal-speed sentinel, not a finite non-zero and not a
     * legal zero. One snapshot only. */
    stopped = false;
    moving = false;
    {
        bool any_nonzero = false;
        bool any_zero = false;
        bool illegal = s->speed_invalid;
        bool snapshot = s->adapter_verified && s->clock_verified && s->sample_present;
        bool shift_drn = (s->shift == PK_SHIFT_D || s->shift == PK_SHIFT_R ||
                          s->shift == PK_SHIFT_N);
        bool shift_parked = (s->shift == PK_SHIFT_P);
        if (s->speed_present && s->speed > 0) {
            any_nonzero = true;
            nspeed++;
        } else if (s->speed_present && s->speed == 0) {
            any_zero = true;
            nspeed++;
        } else if (s->speed_present && s->speed < 0) {
            illegal = true;
        }
        if (s->speed_float_present && s->speed_float > 0) {
            any_nonzero = true;
            nspeed++;
        } else if (s->speed_float_present && s->speed_float == 0) {
            any_zero = true;
            nspeed++;
        } else if (s->speed_float_present && s->speed_float < 0) {
            illegal = true;
        }
        /* D/R/N and a finite non-zero speed are motion even when another
         * speed is illegal. Illegal speed still forbids Park. */
        if (snapshot && (shift_drn || any_nonzero)) {
            moving = true;
        } else if (snapshot && !illegal && shift_parked && nspeed > 0 &&
                   any_zero && !any_nonzero) {
            stopped = true;
        }
    }
    if (moving) {
        app->moving = true;
        app->motion = PK_MOTION_TENDENCY;
        app->motion_until = app->now + PK_PARK_FRESH_MS;
    } else {
        pk_app_motion_unknown(app);
    }

    {
        /* Only an explicitly verified P plus a present legal zero on this
         * same trusted snapshot may qualify. Vehicle wall-clock stamps are
         * not comparable with the session uptime clock and are not used. */
        proof = stopped;
    }
    parked = proof;
    lock_proof = s->locked_present && s->clock_verified &&
                 s->adapter_verified && s->lock_sample_present;

    pk_grant_clear(&app->grant);
    app->grant.scope = pk_app_scope(app);
    app->grant.read_id = s->request_id;
    app->grant.requested = requested_at;
    app->grant.received = app->now;
    app->grant.sample_present = s->sample_present;
    app->grant.sample = s->sample;
    app->grant.shift_present = (s->shift != PK_SHIFT_ABSENT);
    app->grant.speed_present = nspeed > 0;
    app->grant.locked_present = s->locked_present;
    app->grant.parked = parked;
    app->grant.proof = proof;
    if (proof) {
        /* Lease is a product-candidate cap, not a measured safety TTL. */
        app->grant.until = grant_lease_until(app->now, PK_PARK_LEASE_MS);
    } else {
        app->grant.until = app->now;
    }
    if (lock_proof) {
        app->grant.locked_known = true;
        app->grant.locked = s->locked;
        app->grant.lock_until = grant_lease_until(app->now, PK_LOCK_LEASE_MS);
    } else {
        app->grant.locked_known = false;
        app->grant.lock_until = app->now;
    }
    app->has_grant = true;

    finish_qualify_outcome(app, parked);
    return parked;
}
