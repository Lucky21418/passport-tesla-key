#ifndef PK_APP_INTERNAL_H
#define PK_APP_INTERNAL_H

#include "pk_app.h"

/* Internal helpers shared by application-layer modules. Not a public ABI. */

void pk_app_set_msg(pk_app_t *app, pk_msg_id_t id);
void pk_app_motion_unknown(pk_app_t *app);
bool pk_app_known_motion(const pk_app_t *app);
/* Record what the radio actually did. A stale delivered flag cannot
 * outvote a failed return code. */
pk_tx_outcome_t pk_app_note_send(pk_app_t *app, pk_err_t rc);
void pk_app_set_msg_text(pk_app_t *app, pk_msg_id_t id, const char *extra);
void pk_app_invalidate(pk_app_t *app, pk_msg_id_t why);
void pk_app_bump_generation(pk_app_t *app);
void pk_app_set_busy(pk_app_t *app, bool busy);
void pk_app_clear_auth_state(pk_app_t *app);
void pk_app_log(pk_app_t *app, pk_log_event_t ev, pk_log_err_t err);
pk_intent_t pk_app_make_intent(pk_app_t *app, pk_action_t action);
pk_action_t pk_app_primary(const pk_app_t *app);
const char *pk_app_guard(pk_app_t *app, pk_action_t action, const pk_intent_t *params);
bool pk_app_commit(pk_app_t *app, const pk_intent_t *intent, pk_ms_t t);
/* Phase budget clipped to the gesture's frozen intent_deadline.
 * A zero intent_deadline leaves the phase budget unchanged. */
pk_ms_t pk_app_phase_deadline(const pk_app_t *app, pk_ms_t now, pk_ms_t budget);
bool pk_app_navigate(pk_app_t *app, pk_page_t page);

void pk_arbiter_init(pk_arbiter_t *a);
void pk_arbiter_invalidate(pk_arbiter_t *a);
void pk_keys_dot_press(pk_app_t *app, pk_ms_t t);
void pk_keys_dot_release(pk_app_t *app, pk_ms_t t);
void pk_keys_tick(pk_app_t *app, pk_ms_t t);
void pk_keys_commit_click_if_due(pk_app_t *app, pk_ms_t t);
bool pk_side_press(pk_app_t *app, pk_key_id_t key, pk_ms_t t);
void pk_side_release(pk_app_t *app, pk_key_id_t key, pk_ms_t t);
void pk_side_tick(pk_app_t *app, pk_ms_t t);
void pk_power_press(pk_app_t *app, pk_ms_t t);
void pk_power_release(pk_app_t *app, pk_ms_t t);

void pk_app_expire_state(pk_app_t *app, pk_ms_t t);
void pk_app_sleep(pk_app_t *app);
void pk_app_exit_wait(pk_app_t *app);
bool pk_app_start_reconnect(pk_app_t *app, bool manual);
bool pk_app_reconnect_attempt(pk_app_t *app, bool result);
void pk_app_cancel_reconnect(pk_app_t *app);

void pk_app_present_wake_offer(pk_app_t *app, pk_msg_id_t why);
bool pk_app_accept_qualification_offer(pk_app_t *app);
void pk_app_decline_qualification_offer(pk_app_t *app);
/*
 * wake=true is accepted only when the caller holds a user-gesture token
 * from the "唤醒车辆并检查" focus choice. Internal / test direct calls
 * without that token are rejected (C01 / M4).
 */
bool pk_app_request_qualification(pk_app_t *app, bool wake, bool user_selected_wake);
bool pk_app_start_lock_verification(pk_app_t *app, pk_cmd_result_t command_result);
bool pk_app_require_mgmt(pk_app_t *app, pk_mgmt_op_t op, bool current);

void pk_app_open_status(pk_app_t *app, pk_origin_t origin);
/* One-shot silent read of the charge and closures sections: refreshes
 * capability state (port/sentry) without leaving the current page. */
bool pk_app_probe_caps(pk_app_t *app);
/* Freeze the current intent and wake the car once (VCSEC), then settle and
 * replay the intent. Only user-gesture, Infotainment-domain commands use it. */
bool pk_app_start_wake_then(pk_app_t *app);
/* Screen takeover: capture the interrupted page once, then show WAIT/RESULT.
 * Nested takeovers never overwrite the original capture. */
void pk_app_present_modal(pk_app_t *app, pk_page_t page);
void pk_app_exit_status(pk_app_t *app);
/* Fault 25: mark this section too large and queue the next fitting section. */
bool pk_app_status_skip_section(pk_app_t *app);
/* Consume one section reply; queues the next section or completes the page. */
bool pk_app_status_advance(pk_app_t *app);
/* App-task half: send the queued section outside the BLE RX callback. */
void pk_app_status_pump(pk_app_t *app);
bool pk_app_activate_setting(pk_app_t *app, pk_setting_id_t key);
void pk_app_begin_editor(pk_app_t *app, pk_setting_id_t key);
void pk_app_settings_arrow(pk_app_t *app, int delta);
bool pk_app_editor_select(pk_app_t *app);
bool pk_app_save_editor(pk_app_t *app);
void pk_app_cancel_editor(pk_app_t *app);
bool pk_app_confirm_reset(pk_app_t *app, bool confirmed);

int pk_app_list_items(const pk_app_t *app, int *out, int max);
void pk_app_list_arrow(pk_app_t *app, pk_key_id_t key);
bool pk_app_select_list_focus(pk_app_t *app);
bool pk_app_select_vehicle(pk_app_t *app, int vehicle);
bool pk_app_enter_vin(pk_app_t *app);
void pk_app_close_ap(pk_app_t *app, const char *reason);
bool pk_app_submit_vin(pk_app_t *app, const char *text);
bool pk_app_pair_start(pk_app_t *app, const char *target_vin,
                       bool candidate_identity_match, bool user_confirmed);
bool pk_app_pair_next(pk_app_t *app);
void pk_app_pair_cancel(pk_app_t *app);
void pk_app_pair_release_radio(pk_app_t *app);
bool pk_app_delete_vehicle(pk_app_t *app, int target, bool reset, pk_store_crash_t crash);

/* M3: one-shot BLE. Transport may be NULL (PK_ERR_NOT_WIRED). */
pk_err_t pk_ble_send_once(pk_app_t *app, pk_domain_t domain, pk_action_t action, pk_ms_t deadline);
/* Status page walks one VehicleData section per signed request. */
pk_err_t pk_ble_send_status_step(pk_app_t *app, pk_ms_t deadline);
/* Negotiated ATT MTU, or the planner's protocol fallback when unknown. */
uint16_t pk_ble_effective_mtu(const pk_app_t *app);
pk_err_t pk_ble_send_once_to(pk_app_t *app, const pk_scope_t *scope,
                             pk_domain_t domain, pk_action_t action, pk_ms_t deadline);
void pk_ble_invalidate_in_flight(pk_app_t *app);
void pk_ble_release_in_flight(pk_app_t *app);
bool pk_ble_match(const pk_app_t *app, pk_id_t txn, const pk_scope_t *scope);

#define PK_LIST_ADD      (-10)
#define PK_LIST_STATUS   (-11)
#define PK_LIST_SETTINGS (-12)
#define PK_LIST_QUICK    (-13)

bool pk_action_is_park_gated(pk_action_t a);
bool pk_setting_is_local(pk_setting_id_t k);
pk_action_t pk_setting_action(pk_setting_id_t k);
const pk_setting_id_t *pk_app_quick_settings(int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* PK_APP_INTERNAL_H */
