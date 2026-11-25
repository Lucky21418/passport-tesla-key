#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_power.h"
#include "pk_plan.h"
#include "pk_protocol.h"

#include <string.h>

void pk_app_set_msg(pk_app_t *app, pk_msg_id_t id)
{
    if (!app) {
        return;
    }
    app->message = id;
    app->message_extra[0] = '\0';
}

void pk_app_set_msg_text(pk_app_t *app, pk_msg_id_t id, const char *extra)
{
    if (!app) {
        return;
    }
    app->message = id;
    app->message_extra[0] = '\0';
    if (extra) {
        strncpy(app->message_extra, extra, sizeof(app->message_extra) - 1);
    }
}

void pk_arbiter_init(pk_arbiter_t *a)
{
    if (!a) {
        return;
    }
    memset(a, 0, sizeof(*a));
}

void pk_arbiter_invalidate(pk_arbiter_t *a)
{
    if (!a) {
        return;
    }
    a->deadline_valid = false;
    a->deadline = 0;
    a->has_intent = false;
    pk_intent_clear(&a->intent);
    a->consumed = true;
}

void pk_app_bump_generation(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->generation++;
    pk_arbiter_invalidate(&app->keys);
}

void pk_app_set_busy(pk_app_t *app, bool busy)
{
    if (!app || app->busy == busy) {
        return;
    }
    app->busy = busy;
    pk_app_bump_generation(app);
}

void pk_app_clear_auth_state(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->auth = PK_MSG_AUTH_UNCONFIRMED;
    app->auth_until = 0;
    app->reconnect.active = false;
    app->addkey_requests_driver = false;
    app->role_observed = false;
    app->role_is_driver = false;
    app->identity_binding_verified = false;
    app->session_vcsec_proven = false;
    app->session_infotainment_proven = false;
}

void pk_app_log(pk_app_t *app, pk_log_event_t ev, pk_log_err_t err)
{
    if (!app) {
        return;
    }
    pk_log_append(&app->logs, ev, app->vehicle != PK_VEHICLE_NONE, app->now, err);
}

void pk_app_invalidate(pk_app_t *app, pk_msg_id_t why)
{
    if (!app) {
        return;
    }
    if (why != PK_MSG_QUAL_IN_PROGRESS) {
        app->has_recheck = false;
        app->wake_choice_available = false;
        app->has_deferred = false;
        /* A live status-wake total survives disconnect. Reconnect must not
         * buy another wake or start a read past that original instant. */
        if (app->wake_owner != PK_WAKE_OWNER_STATUS ||
            app->intent_deadline == 0 ||
            pk_time_due(app->now, app->intent_deadline)) {
            app->intent_deadline = 0;
        }
    }
    pk_app_bump_generation(app);
    app->status_too_large_mask = 0;
    pk_grant_clear(&app->grant);
    app->has_grant = false;
    app->field_count = 0;
    /* Cancel / release the transport slot while pending.id is still
     * valid. Clearing has_pending first made cancel dead code and left
     * the adapter in_flight stuck.
     */
    pk_ble_invalidate_in_flight(app);
    app->has_pending = false;
    memset(&app->pending, 0, sizeof(app->pending));
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    app->result_auto_return = false;
    app->side.active = false;
    app->busy = false;
    pk_app_set_msg(app, why);
}

void pk_app_present_modal(pk_app_t *app, pk_page_t page)
{
    if (!app || (page != PK_PAGE_WAIT && page != PK_PAGE_RESULT)) {
        return;
    }
    if (app->page != PK_PAGE_WAIT && app->page != PK_PAGE_RESULT) {
        app->modal_return_page = app->page; /* capture once, at presentation */
    }
    pk_arbiter_invalidate(&app->keys);
    app->page = page;
}

bool pk_app_navigate(pk_app_t *app, pk_page_t page)
{
    if (!app) {
        return false;
    }
    if (pk_app_known_motion(app) && (page == PK_PAGE_CARS || page == PK_PAGE_SETTINGS ||
                        page == PK_PAGE_VIN || page == PK_PAGE_PAIR ||
                        page == PK_PAGE_DELETE_CONFIRM)) {
        pk_app_set_msg(app, PK_MSG_DRV_MANAGEMENT);
        return false; /* refuse the gesture; never move the user */
    }
    if (app->page == PK_PAGE_STATUS && page != PK_PAGE_STATUS) {
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        app->field_count = 0;
        app->origin = PK_ORIGIN_LIST;
    }
    if ((app->page == PK_PAGE_VIN || app->page == PK_PAGE_PAIR) && page != app->page) {
        pk_app_pair_cancel(app);
    }
    if (page != PK_PAGE_SETTINGS) {
        app->edit_open = false;
        app->editing = false;
        app->edit_key = PK_SETTING_BRIGHTNESS;
        memset(&app->draft, 0, sizeof(app->draft));
        app->has_recheck = false;
    }
    if (page != PK_PAGE_WAIT && page != PK_PAGE_RESULT) {
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        app->lock_alert_active = false;
        app->result_auto_return = false;
    }
    if (page == PK_PAGE_CARS) {
        if (app->vehicle >= 0 && pk_store_slot_used(&app->store, app->vehicle)) {
            app->car_focus = app->vehicle;
        } else if (pk_store_count(&app->store) < PK_MAX_VEHICLES) {
            app->car_focus = PK_LIST_ADD;
        } else {
            app->car_focus = 0;
        }
    }
    pk_arbiter_invalidate(&app->keys);
    app->page = page;
    return true;
}

void pk_app_sleep(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->awake = false;
    app->lock_alert_active = false;
    pk_grant_clear(&app->grant);
    app->has_grant = false;
    pk_app_bump_generation(app);
    app->side.active = false;
    /* Screen sleep is not navigation out of VIN; AP keeps its 120s cap. */
    if (app->page != PK_PAGE_VIN) {
        pk_app_close_ap(app, "screen-sleep");
    }
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    app->has_deferred = false;
    app->intent_deadline = 0;
    app->busy = false;
    app->field_count = 0;
    if (app->hal.backlight && app->hal.backlight->set_awake) {
        (void)app->hal.backlight->set_awake(app->hal.backlight_ctx, false);
    }
}

void pk_app_set_connected(pk_app_t *app, bool connected)
{
    bool had_inflight = false;
    bool inflight_lock = false;
    pk_tx_outcome_t inflight_outcome = PK_TX_NOT_SENT;

    if (!app || app->connected == connected) {
        return;
    }
    if (!connected && app->has_pending) {
        had_inflight = true;
        inflight_lock = (app->pending.action == PK_ACTION_LOCK ||
                         app->pending.kind == PK_PENDING_LOCK_VERIFY);
        inflight_outcome = app->last_tx_outcome;
        if (app->message == PK_MSG_SENT_WAIT && inflight_outcome == PK_TX_NOT_SENT) {
            inflight_outcome = PK_TX_SENT_UNKNOWN;
        }
    }
    app->connected = connected;
    app->connection++;
    app->status_too_large_mask = 0;
    app->vcsec_status_valid = false;
    app->vcsec_asleep = false;
    app->vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
    app->vcsec_observed_valid = false;
    app->vcsec_observed_asleep = false;
    app->vcsec_observed_lock_state = PK_LOCK_READ_UNKNOWN;
    /* A new link has no fresh snapshot. moving==false is unknown, not parked. */
    pk_app_motion_unknown(app);
    if (app->hal.ble == pk_ble_adapter_transport() && app->hal.ble_ctx) {
        ((pk_ble_adapter_t *)app->hal.ble_ctx)->connected_slot =
            connected ? app->vehicle : PK_VEHICLE_NONE;
    }
    pk_app_invalidate(app, PK_MSG_SWITCHED);
    app->auth = PK_MSG_AUTH_UNCONFIRMED;
    app->auth_until = 0;
    if (connected) {
        app->reconnect.active = false;
        if (app->page == PK_PAGE_OFFLINE) {
            app->page = PK_PAGE_MAIN; /* reconnect flow completed */
        }
        pk_app_set_msg(app, PK_MSG_RECONNECTED);
        return;
    }
    pk_proto_aes128_gcm_clear_cache();
    pk_app_caps_clear(&app->caps);
    if (app->vehicle != PK_VEHICLE_NONE && !app->ap &&
        app->page != PK_PAGE_PAIR && !app->pair_radio_held && !app->low_battery) {
        (void)pk_app_start_reconnect(app, false);
    }
    if (!had_inflight) {
        return;
    }
    /* Reconnect may keep the link, but it must not restore the old write.
     * Set the terminal after start_reconnect so NO_REPLAY does not hide it. */
    if (inflight_outcome == PK_TX_NOT_SENT) {
        pk_app_set_msg(app, PK_MSG_CANCELLED);
        return;
    }
    pk_app_set_msg(app, inflight_lock ? PK_MSG_LOCK_UNCONFIRMED : PK_MSG_CMD_UNCONFIRMED);
    if (inflight_lock) {
        pk_app_present_modal(app, PK_PAGE_RESULT);
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        app->lock_alert_active = true;
        app->lock_alert_until = app->now + PK_LOCK_ALERT_DISPLAY_MS;
        app->awake = true;
        app->last_activity = app->now;
    }
}

void pk_app_vcsec_status(pk_app_t *app, bool asleep, pk_lock_read_t lock_state)
{
    if (!app) {
        return;
    }
    app->vcsec_asleep = asleep;
    if (lock_state != PK_LOCK_READ_UNKNOWN) {
        app->vcsec_status_valid = true;
        app->vcsec_reconcile_attempts = 0u;
        app->vcsec_reconcile_next_ms = 0u;
        app->vcsec_lock_state = lock_state;
    }
    if (app->has_pending && app->pending.action == PK_ACTION_VCSEC_GET_STATUS_ONCE &&
        (app->pending.kind == PK_PENDING_UNLOCK_VERIFY ||
         app->pending.kind == PK_PENDING_COMMAND)) {
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
        if (app->pending.kind == PK_PENDING_UNLOCK_VERIFY) {
            app->result_auto_return = false;
            if (lock_state == PK_LOCK_READ_UNLOCKED) {
                pk_app_set_msg(app, PK_MSG_CMD_ACCEPTED);
            } else {
                pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
            }
        } else {
            pk_app_set_msg(app, PK_MSG_CMD_ACCEPTED);
        }
        if (app->low_battery) {
            app->last_activity = app->now;
        }
    }
}

void pk_app_vcsec_status_observed(pk_app_t *app, bool asleep, pk_lock_read_t lock_state)
{
    if (!app) {
        return;
    }
    /* S2 §1: Non-authoritative Legacy observation updates display-only fields
     * ONLY; it NEVER sets vcsec_status_valid / vcsec_lock_state / vcsec_asleep
     * (which feed pk_app_can_vcsec_unlock / pk_app_primary) and NEVER confirms
     * lock or unlock verification as accepted. */
    app->vcsec_observed_valid = true;
    app->vcsec_observed_asleep = asleep;
    app->vcsec_observed_lock_state = lock_state;
    if (app->has_pending && app->pending.action == PK_ACTION_VCSEC_GET_STATUS_ONCE &&
        (app->pending.kind == PK_PENDING_UNLOCK_VERIFY ||
         app->pending.kind == PK_PENDING_COMMAND)) {
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
        app->result_auto_return = false;
        pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
        if (app->low_battery) {
            app->last_activity = app->now;
        }
    }
}

void pk_app_protocol_fault_ex(pk_app_t *app, int fault, bool from_vcsec, bool authenticated)
{
    pk_pending_kind_t kind = PK_PENDING_NONE;
    pk_action_t action = PK_ACTION_NONE;

    if (!app) {
        return;
    }
    if (app->has_pending) {
        kind = app->pending.kind;
        action = app->pending.action;
    }
    /* Fault 25 is per-section, not per-page: skip it and keep reading the
     * remaining sections instead of retrying the same oversized request.
     * Only this vehicle-confirmed path learns an exclusion for the rest of
     * the connection; a local MTU shrink is temporary. */
    if (kind == PK_PENDING_STATUS &&
        fault == PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED) {
        if (app->status_step < (uint8_t)PK_STATUS_SECTION_COUNT) {
            app->status_too_large_mask |= (uint8_t)(1u << app->status_step);
        }
        if (pk_app_status_skip_section(app)) {
            return;
        }
    }
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    memset(&app->pending, 0, sizeof(app->pending));
    app->has_deferred = false;
    app->intent_deadline = 0;
    pk_app_set_busy(app, false);
    pk_arbiter_invalidate(&app->keys);
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    app->result_auto_return = false;
    if (from_vcsec &&
        (pk_proto_fault_needs_resync(fault) || pk_proto_fault_is_key_error(fault))) {
        /* Only session/key/counter/epoch desync faults invalidate known lock
         * state. BUSY, TIMEOUT, INSUFFICIENT_PRIVILEGES, and non-resync outer
         * faults preserve known reported state. */
        app->vcsec_status_valid = false;
        app->vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
        app->vcsec_asleep = false;
        app->vcsec_observed_valid = false;
        app->vcsec_observed_lock_state = PK_LOCK_READ_UNKNOWN;
        app->vcsec_observed_asleep = false;
        app->vcsec_reconcile_attempts = 0u;
        app->vcsec_reconcile_next_ms = 0u;
    }
    if (kind == PK_PENDING_STATUS) {
        app->status_phase[0] = '\0';
        strncpy(app->status_phase,
                fault == PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED ? "too_large" : "timeout",
                sizeof(app->status_phase) - 1);
        pk_app_set_msg(app, fault == PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED
                              ? PK_MSG_RESPONSE_TOO_LARGE
                              : PK_MSG_STATUS_TIMEOUT);
        return;
    }
    pk_app_present_modal(app, PK_PAGE_RESULT);
    if (!authenticated) {
        /* S2 §2C: An outer unprotected fault is a weak fault / resync hint,
         * NEVER an authenticated rejection, and preserves unknown outcome for
         * any command that may have been sent. */
        if (action == PK_ACTION_LOCK || kind == PK_PENDING_LOCK_VERIFY) {
            pk_app_set_msg(app, PK_MSG_LOCK_UNCONFIRMED);
        } else if (pk_proto_fault_needs_resync(fault) || pk_proto_fault_is_key_error(fault)) {
            pk_app_set_msg(app, PK_MSG_AUTH_UNCONFIRMED);
        } else if (fault == PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED) {
            pk_app_set_msg(app, PK_MSG_RESPONSE_TOO_LARGE);
        } else {
            pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
        }
    } else if (pk_proto_fault_is_key_error(fault)) {
        pk_app_set_msg(app, PK_MSG_NEED_APPROVAL);
    } else if (fault == PK_PROTO_FAULT_INSUFFICIENT_PRIVILEGES) {
        pk_app_set_msg(app, PK_MSG_AUTH_REJECTED);
    } else if (pk_proto_fault_needs_resync(fault)) {
        pk_app_set_msg(app, PK_MSG_AUTH_UNCONFIRMED);
    } else if (fault == PK_PROTO_FAULT_RESPONSE_MTU_EXCEEDED) {
        pk_app_set_msg(app, PK_MSG_RESPONSE_TOO_LARGE);
    } else if (fault == PK_PROTO_FAULT_BUSY || fault == PK_PROTO_FAULT_TIMEOUT ||
               fault == PK_PROTO_FAULT_INTERNAL) {
        pk_app_set_msg(app, PK_MSG_CMD_TIMEOUT);
    } else {
        pk_app_set_msg(app, PK_MSG_CMD_UNCONFIRMED);
    }
    if (action == PK_ACTION_LOCK || kind == PK_PENDING_LOCK_VERIFY) {
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        app->lock_alert_active = true;
        app->lock_alert_until = app->now + PK_LOCK_ALERT_DISPLAY_MS;
        app->awake = true;
        app->last_activity = app->now;
    }
}

void pk_app_protocol_fault(pk_app_t *app, int fault, bool from_vcsec)
{
    pk_app_protocol_fault_ex(app, fault, from_vcsec, true);
}

bool pk_app_tx_failed(pk_app_t *app, pk_id_t txn_id, pk_tx_outcome_t outcome)
{
    pk_pending_kind_t kind;
    pk_action_t action;

    if (!app) {
        return false;
    }
    app->last_tx_outcome = outcome;
    if (!app->has_pending || (txn_id != 0 && app->pending.id != txn_id)) {
        return false;
    }
    kind = app->pending.kind;
    action = app->pending.action;
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    memset(&app->pending, 0, sizeof(app->pending));
    app->has_deferred = false;
    app->intent_deadline = 0;
    app->wake_then_status = false;
    pk_app_set_busy(app, false);
    pk_arbiter_invalidate(&app->keys);
    if (kind == PK_PENDING_STATUS) {
        strncpy(app->status_phase, "timeout", sizeof(app->status_phase) - 1);
        pk_app_set_msg(app, PK_MSG_STATUS_TIMEOUT);
    } else if (action == PK_ACTION_LOCK || kind == PK_PENDING_LOCK_VERIFY) {
        app->lock_fsm = PK_LOCK_FSM_RESULT;
        pk_app_set_msg(app, (outcome == PK_TX_NOT_SENT)
                                ? PK_MSG_CANCELLED
                                : PK_MSG_LOCK_UNCONFIRMED);
    } else {
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        pk_app_set_msg(app, (outcome == PK_TX_NOT_SENT)
                                ? PK_MSG_CANCELLED
                                : PK_MSG_CMD_UNCONFIRMED);
    }
    return true;
}

void pk_app_ble_slots_full(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->reconnect.active = false;
    if (app->has_pending) {
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
    }
    pk_app_set_msg(app, PK_MSG_BLE_SLOTS_FULL);
}

void pk_app_set_desktop(pk_app_t *app, bool desktop)
{
    if (!app || app->desktop == desktop) {
        return;
    }
    app->desktop = desktop;
}

void pk_app_battery(pk_app_t *app, bool known, int percent, bool low)
{
    bool rising_low;
    if (!app) {
        return;
    }
    rising_low = low && !app->low_battery;
    app->battery_known = known;
    /* Unknown must display "—", never a fabricated 0% (C11 / G-06).
     * No percentage safety floor is invented without a measured profile.
     */
    app->battery_percent = known ? percent : 0;
    app->low_battery = low;
    if (rising_low) {
        pk_power_low_battery_enter(app);
    }
    /* Must not drop a protected driving BLE link, and must not disable LOCK. */
}

void pk_app_reboot(pk_app_t *app)
{
    bool orphan;
    if (!app) {
        return;
    }
    orphan = app->orphan || app->enrollment_sent;
    pk_app_close_ap(app, "reboot");
    pk_app_clear_auth_state(app);
    pk_app_invalidate(app, PK_MSG_REBOOT);
    app->session++;
    app->connected = false;
    pk_app_motion_unknown(app);
    app->busy = false;
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    app->auth = PK_MSG_AUTH_UNCONFIRMED;
    /* connected is already false, so release will drop the candidate
     * radio and will not reconnect the previous car (C08 / P1-6).
     */
    pk_app_pair_release_radio(app);
    pk_scope_clear(&app->pair_scope);
    app->pair_step = 0;
    app->enrollment_sent = false;
    app->has_vin = false;
    app->vin[0] = '\0';
    app->orphan = orphan;
    app->page = PK_PAGE_MAIN;
    pk_arbiter_invalidate(&app->keys);
}

void pk_hal_attach(pk_app_t *app, const pk_hal_t *hal)
{
    if (!app) {
        return;
    }
    if (hal) {
        app->hal = *hal;
        app->store.nvs = hal->nvs;
        app->store.nvs_ctx = hal->nvs_ctx;
        app->store.physical_nvs_attached = (hal->nvs != NULL);
    } else {
        pk_hal_init_unwired(&app->hal);
        app->store.nvs = NULL;
        app->store.nvs_ctx = NULL;
        app->store.physical_nvs_attached = false;
    }
}

void pk_app_init(pk_app_t *app, int initial_vehicle, bool paired)
{
    if (!app) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->now = 0;
    app->last_activity = 0;
    app->time_set = false;
    /* Production default is 0 cars. Do not hard-code a Model 3 as the
     * actual target (C11). initial_vehicle is reserved for a later store
     * hydrate; this phase does not invent a VIN or seed a fake car.
     */
    (void)initial_vehicle;
    (void)paired;
    app->vehicle = PK_VEHICLE_NONE;
    pk_scope_clear(&app->pair_scope);
    app->pair_radio_held = false;
    app->paired = false;
    app->connection = 1;
    app->session = 1;
    app->generation = 1;
    app->connected = false;
    app->busy = false;
    app->awake = true;
    app->page = PK_PAGE_MAIN;
    app->modal_return_page = PK_PAGE_MAIN;
    app->origin = PK_ORIGIN_LIST;
    app->focus = PK_SETTING_BRIGHTNESS;
    app->offer_focus = PK_OFFER_CHECK;
    app->brightness = PK_BRIGHTNESS_DEFAULT;
    app->power = PK_POWER_COMFORT;
    app->delete_focus = 0; /* cancel is the default */
    app->reset_focus = 0;
    app->car_focus = PK_LIST_ADD;
    app->battery_known = false;
    pk_app_caps_clear(&app->caps);
    pk_log_init(&app->logs);
    pk_store_init(&app->store);
    pk_hal_init_unwired(&app->hal);
    pk_arbiter_init(&app->keys);
    pk_app_set_msg(app, PK_MSG_EMPTY);
}
