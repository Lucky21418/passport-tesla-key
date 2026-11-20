#include "pk_app_internal.h"
#include "pk_plan.h"
#include "pk_protocol.h"

#include <string.h>

/*
 * M3: one-shot BLE semantics. This is the only module allowed to touch a
 * Tesla protocol stack, and only through pk_ble_transport_t.
 *
 * Forbidden:
 *  - SDK built-in retries in Vehicle.Send, StartSession (ShouldRetry /
 *    RetryInterval, 1 s), getVCSECResult loops
 *  - Cloud / Fleet wakeup (Wakeup() when FleetAPI is present)
 *  - Infinite handshake
 *  - Treating send-success or the first reply as terminal accept or lock confirm
 *  - Auto-reconnect just to finish a lock verification (see M6)
 *
 * Allowed wake path is only M4's explicit user choice, via
 * RKE_ACTION_WAKE_VEHICLE on BLE (wakeupRKE), never a cloud wake.
 *
 * One in-flight request per VCSEC domain. LOCK and GET_STATUS must not
 * run concurrently. Late, duplicate, or post-timeout packets must not
 * update a new transaction, the home screen, or trigger sound.
 *
 * 待真机: retry-disabled delivery on the 2023 refreshed Model 3 and
 * rental 3/Y, MTU / segmentation, session rebuild cost, BLE+SoftAP
 * coexistence. Do not advertise zero-failure or zero-energy.
 */

typedef struct {
    bool in_flight;
    pk_domain_t domain;
    pk_id_t txn;
    pk_scope_t scope;
    pk_ms_t deadline;
    pk_action_t action;
} pk_ble_slot_t;

/* The single VCSEC in-flight slot lives in the app via pending + this file
 * static only if we had a singleton — we do not. Track it on the pending
 * record and a small field in message_extra is wrong. Use pending +
 * pair_id as the two possible VCSEC occupants.
 */

static bool action_uses_vcsec(pk_action_t a)
{
    return a == PK_ACTION_LOCK || a == PK_ACTION_UNLOCK ||
           a == PK_ACTION_FRUNK_OPEN || a == PK_ACTION_TRUNK_MOVE ||
           a == PK_ACTION_VCSEC_WAKE_ONCE ||
           a == PK_ACTION_VCSEC_GET_STATUS_ONCE || a == PK_ACTION_ADDKEY ||
           a == PK_ACTION_PAIR_VERIFY_VCSEC;
}

/* Occupied by a different VCSEC occupant than the txn being sent now.
 * The current pending (app->serial) is allowed so GET_STATUS can follow
 * a finished LOCK, and pair-step-4 verify can start after AddKey.
 */
static bool vcsec_blocked(const pk_app_t *app, pk_action_t action)
{
    if (app->has_pending && action_uses_vcsec(app->pending.action)) {
        if (!(app->pending.id == app->serial && app->pending.action == action)) {
            return true;
        }
    }
    if (app->enrollment_sent && app->pair_step >= 3 &&
        action != PK_ACTION_PAIR_VERIFY_VCSEC) {
        return true;
    }
    return false;
}

void pk_ble_release_in_flight(pk_app_t *app)
{
    if (!app || !app->hal.ble || !app->hal.ble->cancel) {
        return;
    }
    /* Adapter cancel(txn=0) clears any stuck one-shot slot. */
    (void)app->hal.ble->cancel(app->hal.ble_ctx, 0);
}

void pk_ble_invalidate_in_flight(pk_app_t *app)
{
    pk_id_t txn = 0;

    if (!app) {
        return;
    }
    /* In-flight work is dropped on switch, delete, disconnect, session
     * rebuild, or cancel. Call this BEFORE clearing has_pending so a
     * specific txn can be named; then always release the adapter slot.
     * The transport cancel hook is best-effort; a late packet must still
     * fail pk_ble_match.
     */
    if (app->has_pending) {
        txn = app->pending.id;
    }
    if (app->hal.ble && app->hal.ble->cancel) {
        if (txn != 0) {
            (void)app->hal.ble->cancel(app->hal.ble_ctx, txn);
        }
        (void)app->hal.ble->cancel(app->hal.ble_ctx, 0);
    }
}

bool pk_ble_match(const pk_app_t *app, pk_id_t txn, const pk_scope_t *scope)
{
    pk_scope_t cur;
    if (!app || !app->has_pending || !scope) {
        return false;
    }
    if (app->pending.id != txn) {
        return false;
    }
    if (pk_time_due(app->now, app->pending.deadline)) {
        return false;
    }
    cur = pk_app_scope(app);
    /* Need vehicle + domain connection/session + the frozen txn scope.
     * "BLE came from the same car" is not enough (C09 / M2 / LOCK 2.1).
     * 待真机: official routing-address / request-hash correlation on this
     * BLE stack. If association cannot be proven, do not confirm.
     */
    return pk_scope_same_vehicle_conn_session(&app->pending.scope, &cur) &&
           pk_scope_same_vehicle_conn_session(scope, &app->pending.scope);
}

static pk_err_t send_built(pk_app_t *app, const pk_scope_t *scope,
                           pk_domain_t domain, pk_action_t action, pk_ms_t deadline,
                           uint8_t detail, const uint8_t *payload, size_t payload_len)
{
    pk_ble_frame_t frame;

    if (!app) {
        return PK_ERR_INVALID;
    }
    if (domain == PK_DOMAIN_VCSEC && vcsec_blocked(app, action)) {
        /* A different in-flight VCSEC occupant must not be queued behind. */
        return PK_ERR_BUSY;
    }

    memset(&frame, 0, sizeof(frame));
    /* Pairing AddKey / AUTH must use the caller-supplied target scope
     * (the unsaved candidate), not pk_app_scope() of the current saved car.
     * A NONE vehicle is never a legal write target: do not invent a slot
     * and do not report a vehicle success.
     */
    if (scope) {
        frame.scope = *scope;
    } else {
        frame.scope = pk_app_scope(app);
    }
    if (frame.scope.vehicle == PK_VEHICLE_NONE) {
        return PK_ERR_INVALID;
    }
    frame.domain = domain;
    frame.txn_id = app->serial;
    frame.deadline = deadline;
    frame.action = action;
    frame.detail = detail;
    if (payload && payload_len > 0) {
        if (payload_len > sizeof(frame.payload)) {
            return PK_ERR_INVALID;
        }
        memcpy(frame.payload, payload, payload_len);
        frame.payload_len = payload_len;
    } else {
        frame.payload_len = pk_proto_encode_action_payload(
            action, &app->draft, NULL, frame.payload, sizeof(frame.payload));
    }

    if (action == PK_ACTION_VCSEC_WAKE_ONCE && domain != PK_DOMAIN_VCSEC) {
        return PK_ERR_INVALID;
    }
    if (!pk_plan_request_fits(action, frame.payload_len)) {
        return PK_ERR_INVALID;
    }

    if (!app->hal.ble || !app->hal.ble->send_once) {
        /* Not wired: record the intent and wait for an injected result.
         * Do not invent a vehicle success.
         */
        return PK_ERR_NOT_WIRED;
    }
    /* The attached transport MUST send once and MUST NOT enable SDK retries. */
    return app->hal.ble->send_once(app->hal.ble_ctx, &frame);
}

pk_err_t pk_ble_send_once_to(pk_app_t *app, const pk_scope_t *scope,
                             pk_domain_t domain, pk_action_t action, pk_ms_t deadline)
{
    return send_built(app, scope, domain, action, deadline, 0, NULL, 0);
}

pk_err_t pk_ble_send_status_step(pk_app_t *app, pk_ms_t deadline)
{
    uint8_t payload[64];
    pk_plan_t plan;
    size_t n;

    if (!app) {
        return PK_ERR_INVALID;
    }
    plan = pk_plan_status_section((int)app->status_step);
    if (!pk_plan_fits_mtu(&plan, pk_ble_effective_mtu(app))) {
        return PK_ERR_BLOCKED;
    }
    n = pk_proto_encode_status_section((int)app->status_step, payload, sizeof(payload));
    if (n == 0 || n > plan.max_request_bytes) {
        return PK_ERR_INVALID;
    }
    return send_built(app, NULL, PK_DOMAIN_INFOTAINMENT, PK_ACTION_STATUS_READ, deadline,
                      app->status_step, payload, n);
}

uint16_t pk_ble_effective_mtu(const pk_app_t *app)
{
    uint16_t mtu = 0;

    if (app && app->hal.ble && app->hal.ble->get_mtu &&
        app->hal.ble->get_mtu(app->hal.ble_ctx, &mtu) &&
        mtu > PK_PLAN_MTU_HEADER) {
        return mtu;
    }
    return PK_PLAN_MTU_FALLBACK;
}

pk_err_t pk_ble_send_once(pk_app_t *app, pk_domain_t domain, pk_action_t action, pk_ms_t deadline)
{
    pk_scope_t cur;

    if (!app) {
        return PK_ERR_INVALID;
    }
    cur = pk_app_scope(app);
    return pk_ble_send_once_to(app, &cur, domain, action, deadline);
}
