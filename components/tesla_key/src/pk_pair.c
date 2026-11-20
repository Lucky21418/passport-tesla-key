#include "pk_app_internal.h"
#include "pk_plan.h"

#include <string.h>

/*
 * M9: VIN, pairing, SoftAP. SoftAP + HTTP + DNS live only in the VIN
 * entry session. Any submit closes all three and wipes the buffer.
 * Cancel / enter pair / leave VIN / low battery / connection-route change /
 * reboot / session expiry all close them. The hotspot hard cap is 120 s;
 * 30 s only blanks the screen, and a VIN session may continue until 120 s.
 * The first key after sleep still only wakes. No hotspot after entering
 * pair.
 *
 * Four steps: 1 confirm VIN / target; 2 place the factory card on the
 * centre-console reader (cup-holder side, not the wireless charger) and
 * only then does the user's ● "下一步" send one AddKey
 * (ROLE_DRIVER, KEY_FORM_FACTOR_UNKNOWN(0)); 3 vehicle approval, no
 * advance without it; 4 ● finish starts bounded VCSEC + Infotainment
 * auth, and only a match is saved. Do not auto-switch the current car.
 *
 * On a successful save, clear enrollment_sent / pair_step / pair_id /
 * approved so a later unrelated VIN cancel is not marked as an orphan.
 * Cancel never revokes a vehicle-side enrollment. The orphan mark applies
 * only to a session that sent enrollment and has not finished verify/save.
 *
 * Do not open B's VIN/pair while protecting A's driving BLE.
 *
 * 待真机: UNKNOWN form-factor compatibility (production pair is blocked
 * without a measured proof), first-target trust, BLE+AP coexistence,
 * approval / verify budgets.
 */

/* Bind the radio to the unsaved pairing candidate so AddKey / dual-domain
 * AUTH can be written. This is not a current-car switch: app->vehicle,
 * app->connected, app->connection and app->session stay
 * on the already-saved car (C08: do not auto-switch; do not drop keep-alive
 * or auto-disconnect a protected driving link). connect_once(NONE) remains
 * illegal; first-pair uses PK_VEHICLE_PAIR_CANDIDATE instead.
 */
static pk_err_t pk_pair_bind_radio(pk_app_t *app)
{
    pk_err_t rc;

    if (!app->hal.ble || !app->hal.ble->connect_once) {
        return PK_ERR_NOT_WIRED;
    }
    rc = app->hal.ble->connect_once(app->hal.ble_ctx, PK_VEHICLE_PAIR_CANDIDATE,
                                    PK_RECONNECT_SCAN_MS);
    if (rc == PK_OK) {
        app->pair_radio_held = true;
    }
    /* A failed or unwired bind is not a vehicle success. */
    return rc;
}

void pk_app_pair_release_radio(pk_app_t *app)
{
    bool held;

    if (!app) {
        return;
    }
    held = app->pair_radio_held;
    app->pair_radio_held = false;
    if (held && app->hal.ble && app->hal.ble->disconnect) {
        (void)app->hal.ble->disconnect(app->hal.ble_ctx, PK_VEHICLE_PAIR_CANDIDATE);
    }
    /* Restore the current saved car's radio if the app still considers
     * that car connected. Do not call pk_app_set_connected: that would
     * bump the connection generation and drop keep-alive.
     */
    if (held && app->vehicle != PK_VEHICLE_NONE &&
        app->vehicle != PK_VEHICLE_PAIR_CANDIDATE &&
        app->hal.ble && app->hal.ble->connect_once) {
        (void)app->hal.ble->connect_once(app->hal.ble_ctx, app->vehicle,
                                         PK_RECONNECT_SCAN_MS);
    }
}

static void pk_pair_prepare_scope(pk_app_t *app)
{
    pk_scope_clear(&app->pair_scope);
    app->pair_scope.vehicle = PK_VEHICLE_PAIR_CANDIDATE;
    app->pair_scope.connection = app->pair_id;
    app->pair_scope.session = app->pair_id;
    app->pair_scope.generation = 0;
}

static pk_err_t pk_pair_send(pk_app_t *app, pk_domain_t domain, pk_action_t action,
                             pk_ms_t deadline)
{
    /* Write against pair_scope, never against the current saved car.
     * The caller releases any prior one-shot slot and binds the
     * candidate once per step, so a dual-domain verify is not cancelled
     * between the two frames.
     */
    return pk_ble_send_once_to(app, &app->pair_scope, domain, action, deadline);
}

void pk_app_close_ap(pk_app_t *app, const char *reason)
{
    if (!app) {
        return;
    }
    if (app->ap) {
        if (app->hal.softap && app->hal.softap->stop) {
            (void)app->hal.softap->stop(app->hal.softap_ctx, reason ? reason : "");
        }
        if (app->hal.softap && app->hal.softap->wipe_submit_buffer) {
            app->hal.softap->wipe_submit_buffer(app->hal.softap_ctx);
        }
    }
    app->ap = false;
}

bool pk_app_enter_vin(pk_app_t *app)
{
    int count;
    int existing;
    if (!app) {
        return false;
    }
    count = pk_store_count(&app->store);
    if (app->low_battery || count >= PK_MAX_VEHICLES ||
        !pk_app_management_allowed(app, PK_MGMT_ADD, true, NULL)) {
        pk_app_set_msg(app, PK_MSG_FULL_OR_PROTECTED);
        return false;
    }
    /* A development preset already counts as a submitted VIN: skip the SoftAP
     * entry and pair directly. The portal below is untouched for every build
     * or case without a preset (first setup, or binding another car later). */
    if (app->has_vin && app->vin[0] != '\0') {
        pk_app_cancel_reconnect(app);
        existing = pk_store_find_vin(&app->store, app->vin);
        if (existing != PK_VEHICLE_NONE) {
            app->vehicle = existing;
            (void)pk_store_switch_current(&app->store, existing);
            app->paired = pk_store_slot_key_ready(&app->store, existing);
            app->page = PK_PAGE_CARS;
            return true;
        }
        if (!pk_app_pair_start(app, app->vin, true, true)) {
            return false;
        }
        app->pair_deadline = app->now + PK_PAIR_APPROVAL_MS;
        return true;
    }
    pk_app_cancel_reconnect(app);
    pk_app_close_ap(app, "new-session");
    app->ap = true;
    app->ap_deadline = app->now + PK_AP_INACTIVE_MS;
    if (app->hal.softap && app->hal.softap->start) {
        (void)app->hal.softap->start(app->hal.softap_ctx, app->ap_deadline);
    } else {
        /* SoftAP is not wired in this phase. State still follows the contract. */
    }
    app->page = PK_PAGE_VIN;
    return true;
}

bool pk_app_submit_vin(pk_app_t *app, const char *text)
{
    char vin[PK_VIN_BUF];
    bool valid;
    if (!app || !app->ap) {
        return false;
    }
    valid = pk_vin_valid(text, vin) &&
            pk_store_find_vin(&app->store, vin) == PK_VEHICLE_NONE &&
            pk_store_count(&app->store) < PK_MAX_VEHICLES;
    pk_app_close_ap(app, "submit");
    if (valid) {
        memcpy(app->vin, vin, PK_VIN_BUF);
        app->has_vin = true;
        pk_app_set_msg(app, PK_MSG_VIN_OK);
    } else {
        app->has_vin = false;
        app->vin[0] = '\0';
        pk_app_set_msg(app, PK_MSG_VIN_BAD);
    }
    return valid;
}

bool pk_app_pair_start(pk_app_t *app, const char *target_vin,
                       bool candidate_identity_match, bool user_confirmed)
{
    if (!app || !app->has_vin || !target_vin || strcmp(app->vin, target_vin) != 0 ||
        !candidate_identity_match || !user_confirmed ||
        !pk_app_management_allowed(app, PK_MGMT_PAIR, true, NULL)) {
        pk_app_set_msg(app, PK_MSG_TARGET_UNVERIFIED);
        return false;
    }
    pk_app_close_ap(app, "enter-pair");
    app->page = PK_PAGE_PAIR;
    app->serial++;
    app->pair_id = app->serial;
    app->pair_step = 1;
    app->enrollment_sent = false;
    app->approved = false;
    app->pair_radio_held = false;
    pk_pair_prepare_scope(app);
    return true;
}

bool pk_app_pair_next(pk_app_t *app)
{
    if (!app) {
        return false;
    }
    if (app->pair_step == 1) {
        app->pair_step = 2;
        return true;
    }
    if (app->pair_step == 2) {
        if (app->caps.pair_unknown_form_supported == PK_CAP_NO) {
            pk_app_set_msg(app, PK_MSG_FORM_UNVERIFIED);
            return false;
        }
        /* ROLE_DRIVER, KEY_FORM_FACTOR_UNKNOWN(0). Do not impersonate
         * iOS / NFC / Owner. One AddKey only, and never auto-resend on
         * reject / full / timeout / cancel / verify fail / reboot.
         */
        pk_ble_release_in_flight(app);
        (void)pk_pair_bind_radio(app);
        {
            pk_err_t send_rc = pk_pair_send(app, PK_DOMAIN_VCSEC, PK_ACTION_ADDKEY,
                                            app->now + PK_PAIR_APPROVAL_MS);
            /* S2 §2E: If RNG/keygen failed or candidate pubkey is missing/invalid,
             * block AddKey explicitly: never advance to step 3 / mark orphan /
             * quietly time out waiting for a key that was never sent. */
            if (send_rc == PK_ERR_INVALID) {
                pk_app_pair_release_radio(app);
                app->enrollment_sent = false;
                app->orphan = false;
                pk_app_set_msg(app, PK_MSG_PAIR_REJECTED);
                return false;
            }
            if (send_rc == PK_OK || send_rc == PK_ERR_UNKNOWN) {
                /* The encoder asks for Driver. That is a request, not a read. */
                app->addkey_requests_driver = true;
            }
        }
        app->enrollment_sent = true;
        app->orphan = true;
        app->pair_step = 3;
        app->pair_deadline = app->now + PK_PAIR_APPROVAL_MS;
        pk_app_log(app, PK_LOG_EVENT_PAIR, PK_LOG_ERR_NONE);
        return true;
    }
    if (app->pair_step == 3 && app->approved) {
        app->pair_step = 4;
        app->pair_deadline = app->now + pk_plan_deadline(PK_PLAN_PAIR_VERIFY);
        /* Start bounded dual-domain authentication. Enrollment occupancy
         * does not block this session's VCSEC verify send. Results still
         * arrive via pk_app_pair_verify and must all match before save.
         */
        pk_ble_release_in_flight(app);
        (void)pk_pair_bind_radio(app);
        pk_pair_send(app, PK_DOMAIN_VCSEC, PK_ACTION_PAIR_VERIFY_VCSEC,
                     app->pair_deadline);
        pk_pair_send(app, PK_DOMAIN_INFOTAINMENT,
                     PK_ACTION_PAIR_VERIFY_INFOTAINMENT,
                     app->pair_deadline);
        pk_app_log(app, PK_LOG_EVENT_PAIR, PK_LOG_ERR_NONE);
        return true;
    }
    pk_app_set_msg(app, PK_MSG_NEED_APPROVAL);
    return false;
}

bool pk_app_pair_approve(pk_app_t *app, bool value, pk_id_t request_id, bool correlated)
{
    if (!app || app->pair_step != 3 || request_id != app->pair_id || !correlated ||
        pk_time_due(app->now, app->pair_deadline)) {
        return false;
    }
    app->approved = (value == true);
    if (app->approved) {
        app->caps.pair_unknown_form_supported = PK_CAP_YES;
    }
    pk_app_set_msg(app, app->approved ? PK_MSG_APPROVED_NEED_AUTH : PK_MSG_PAIR_REJECTED);
    return app->approved;
}

bool pk_app_pair_verify(pk_app_t *app, const pk_pair_verify_t *v)
{
    bool ok;
    bool in_window;

    if (!app || !v) {
        return false;
    }
    in_window = v->request_id != 0 && v->request_id == app->pair_id &&
                app->pair_step == 4 && pk_time_before(app->now, app->pair_deadline);
    /* Session proof is the two authenticated domains plus the key match.
     * A handshake must not invent a whitelist role or an identity binding. */
    app->session_vcsec_proven = v->vcsec_authenticated;
    app->session_infotainment_proven = v->infotainment_authenticated;
    if (v->role_observed) {
        app->role_observed = true;
        app->role_is_driver = v->role_driver;
    }
    if (v->role_observed && v->identity_binding_verified) {
        app->identity_binding_verified = true;
    }
    ok = in_window && app->approved &&
         v->vcsec_authenticated && v->infotainment_authenticated &&
         v->key_matches;
    if (ok) {
        bool had_active_car = (app->vehicle != PK_VEHICLE_NONE);
        char paired_vin[PK_VIN_BUF];
        memcpy(paired_vin, app->vin, sizeof(paired_vin));
        if (!pk_store_add_verified(&app->store, app->vin, PK_STORE_CRASH_NONE)) {
            pk_app_set_msg(app, PK_MSG_PAIR_SAVE_FAIL);
            /* Keep process flags and the orphan hint; do not claim the
             * vehicle-side enrollment was revoked.
             */
            return false;
        }
        /* Success must clear enrollment flags so a later unrelated VIN
         * cancel is not labelled an orphan (C08 / t811 residual).
         */
        app->orphan = false;
        app->enrollment_sent = false;
        app->pair_step = 0;
        app->pair_id = 0;
        app->approved = false;
        app->pair_deadline = 0;
        app->has_vin = false;
        app->vin[0] = '\0';
        if (!had_active_car) {
            int new_slot = pk_store_find_vin(&app->store, paired_vin);
            if (new_slot >= 0 && new_slot < PK_MAX_VEHICLES) {
                (void)pk_store_switch_current(&app->store, new_slot);
                app->vehicle = new_slot;
                app->paired = pk_store_slot_key_ready(&app->store, new_slot);
                app->car_focus = new_slot;
            }
        }
        pk_app_set_msg(app, PK_MSG_PAIR_SAVED);
        app->page = PK_PAGE_CARS;
        /* Do not auto-switch an already-active vehicle. Release the candidate
         * radio and connect/restore the saved car.
         */
        pk_app_pair_release_radio(app);
        pk_scope_clear(&app->pair_scope);
    } else if (in_window) {
        pk_app_set_msg(app, PK_MSG_PAIR_VERIFY_FAIL);
    }
    return ok;
}

void pk_app_pair_cancel(pk_app_t *app)
{
    bool sent;
    if (!app) {
        return;
    }
    sent = app->enrollment_sent;
    pk_app_close_ap(app, "cancel-pair");
    pk_app_pair_release_radio(app);
    pk_scope_clear(&app->pair_scope);
    app->has_vin = false;
    app->vin[0] = '\0';
    app->pair_step = 0;
    app->pair_id = 0;
    app->enrollment_sent = false;
    app->approved = false;
    app->pair_deadline = 0;
    if (sent) {
        app->orphan = true;
        pk_app_set_msg(app, PK_MSG_ORPHAN);
    }
}

bool pk_app_select_vehicle(pk_app_t *app, int vehicle)
{
    if (!app || !pk_store_slot_used(&app->store, vehicle)) {
        return false;
    }
    /* Short-● on the already-current car is "return home", not a switch.
     * Do this before management_allowed(SWITCH): stale auth state must not
     * trap the user on the list, and the link / keep-alive / session
     * must stay intact.
     */
    if (vehicle == app->vehicle) {
        pk_arbiter_invalidate(&app->keys);
        app->page = PK_PAGE_MAIN;
        return true;
    }
    if (!pk_app_require_mgmt(app, PK_MGMT_SWITCH, true)) {
        return false;
    }
    if (!pk_store_switch_current(&app->store, vehicle)) {
        return false;
    }
    if (app->connected && app->vehicle != PK_VEHICLE_NONE &&
        app->hal.ble && app->hal.ble->disconnect) {
        (void)app->hal.ble->disconnect(app->hal.ble_ctx, app->vehicle);
    }
    pk_app_clear_auth_state(app);
    pk_app_caps_clear(&app->caps);
    pk_app_invalidate(app, PK_MSG_SWITCHED);
    app->connected = false;
    pk_app_motion_unknown(app);
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    app->connection++;
    app->session++;
    app->vehicle = vehicle;
    app->paired = pk_store_slot_key_ready(&app->store, vehicle);
    app->auth = PK_MSG_AUTH_UNCONFIRMED;
    app->page = PK_PAGE_MAIN;
    return true;
}

bool pk_app_delete_vehicle(pk_app_t *app, int target, bool reset, pk_store_crash_t crash)
{
    pk_store_image_t live;
    bool ok;
    bool current;
    int old_vehicle;
    if (!app) {
        return false;
    }
    old_vehicle = app->vehicle;
    current = (target == app->vehicle);
    if (!pk_app_require_mgmt(app, reset ? PK_MGMT_RESET : PK_MGMT_DELETE, current)) {
        return false;
    }
    if ((reset || current) && app->connected && old_vehicle != PK_VEHICLE_NONE &&
        app->hal.ble && app->hal.ble->disconnect) {
        (void)app->hal.ble->disconnect(app->hal.ble_ctx, old_vehicle);
    }
    pk_arbiter_invalidate(&app->keys);
    ok = pk_store_delete(&app->store, target, reset, crash, &live);
    if (crash == PK_STORE_CRASH_PREPARE) {
        pk_app_reboot(app);
        app->vehicle = live.current;
        pk_app_set_msg(app, PK_MSG_DELETE_INCOMPLETE);
        return false;
    }
    if (crash == PK_STORE_CRASH_COMMIT) {
        if (reset || current) {
            pk_app_clear_auth_state(app);
            pk_app_invalidate(app, PK_MSG_DELETE_RECOVERED);
            app->vehicle = live.current;
            app->connected = false;
            pk_app_motion_unknown(app);
            app->lock_fsm = PK_LOCK_FSM_IDLE;
            app->session++;
            app->auth = PK_MSG_AUTH_UNCONFIRMED;
            app->page = PK_PAGE_MAIN;
            app->paired = (app->vehicle != PK_VEHICLE_NONE) &&
                          pk_store_slot_key_ready(&app->store, app->vehicle);
        }
        return true;
    }
    if (reset || current) {
        if (ok) {
            pk_app_clear_auth_state(app);
            pk_app_caps_clear(&app->caps);
            pk_app_invalidate(app, PK_MSG_RECORD_UPDATED);
            app->vehicle = live.current;
            app->connected = false;
            pk_app_motion_unknown(app);
            app->lock_fsm = PK_LOCK_FSM_IDLE;
            app->session++;
            app->auth = PK_MSG_AUTH_UNCONFIRMED;
            app->page = PK_PAGE_MAIN;
            app->paired = (app->vehicle != PK_VEHICLE_NONE) &&
                          pk_store_slot_key_ready(&app->store, app->vehicle);
            pk_log_clear(&app->logs);
        }
    } else if (ok) {
        pk_app_log(app, PK_LOG_EVENT_DELETE, PK_LOG_ERR_NONE);
        /* Related logs for a deleted non-current car are cleared with the
         * store image; the RAM ring is global and already desensitized.
         */
    }
    if (reset && ok) {
        app->brightness = PK_BRIGHTNESS_DEFAULT;
        app->power = PK_POWER_COMFORT;
        pk_app_log(app, PK_LOG_EVENT_RESET, PK_LOG_ERR_NONE);
    }
    (void)pk_app_navigate(app, PK_PAGE_MAIN);
    return ok;
}
