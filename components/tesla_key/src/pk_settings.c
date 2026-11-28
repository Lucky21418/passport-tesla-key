#include "pk_app_internal.h"

#include <string.h>

/*
 * M8: 18-row same-page settings. Editing is an in-page component, not a
 * new navigation level. A short-● executes a normal row; a double-● returns.
 * Dangerous rows (vent, charge port, reset) require a 500 ms long press, so a
 * stray tap cannot move a closure. Editor cancel is a double-●, not a hold.
 *
 * Current (A) and limit (%) are independent submits. Seat heat requires an
 * explicit seat and level; no all-seat macro. Steering heat uses the old
 * bool command only when that interface is verified. Defog is an independent
 * SetPreconditioningMax(true, true) and does not also send ClimateOn.
 *
 * Driving entry: the existing 500 ms long-● settings page remains reachable
 * during known motion for explicit one-shot climate/defog per D09; vehicle
 * management pages (cars, VIN, pairing, delete) stay blocked while moving.
 *
 * Audio toggle stays hidden.
 */

static const pk_setting_id_t k_basic_settings[9] = {
    PK_SETTING_BRIGHTNESS,
    PK_SETTING_POWER,
    PK_SETTING_FLASH,
    PK_SETTING_HORN,
    PK_SETTING_CLIMATE,
    PK_SETTING_DEFOG,
    PK_SETTING_LOGS,
    PK_SETTING_HOTSPOT,
    PK_SETTING_RESET
};

const pk_setting_id_t *pk_app_quick_settings(int *out_count)
{
    if (out_count) {
        *out_count = (int)(sizeof(k_basic_settings) / sizeof(k_basic_settings[0]));
    }
    return k_basic_settings;
}

static int editor_field_count(pk_setting_id_t key)
{
    return (key == PK_SETTING_SEAT) ? 2 : 1;
}

static int editor_item_count(pk_setting_id_t key)
{
    return editor_field_count(key) + 2; /* fields + save + cancel */
}

/* Cycle a discrete inclusive range. An unset / out-of-range draft plus
 * delta>0 lands on the first option; delta<0 lands on the last.
 */
static bool cycle_int(int *dst, int lo, int hi, int step, int delta, bool have)
{
    int n;
    int idx;

    if (!dst || lo > hi || step <= 0) {
        return false;
    }
    n = ((hi - lo) / step) + 1;
    if (!have || *dst < lo || *dst > hi || ((*dst - lo) % step) != 0) {
        *dst = (delta < 0) ? (lo + (n - 1) * step) : lo;
        return true;
    }
    idx = (*dst - lo) / step;
    idx += (delta < 0) ? -1 : 1;
    while (idx < 0) {
        idx += n;
    }
    idx %= n;
    *dst = lo + idx * step;
    return true;
}

static void persist_live_store(pk_app_t *app)
{
    (void)pk_store_save_prefs(&app->store, app->brightness, app->power);
}

static int effective_seat_positions(const pk_caps_t *caps, int out_pos[8])
{
    int i;
    if (caps && caps->seat_position_count > 0) {
        int n = caps->seat_position_count > 8 ? 8 : caps->seat_position_count;
        for (i = 0; i < n; i++) {
            out_pos[i] = caps->seat_positions[i];
        }
        return n;
    }
    for (i = 0; i < 5; i++) {
        out_pos[i] = i;
    }
    return 5;
}

static int find_seat_pos_index(const pk_caps_t *caps, int pos)
{
    int positions[8];
    int n = effective_seat_positions(caps, positions);
    int i;
    for (i = 0; i < n; i++) {
        if (positions[i] == pos) {
            return i;
        }
    }
    return -1;
}

static int cycle_pair_val(int cur, int first, int second, int delta)
{
    if (cur != first && cur != second) {
        return (delta < 0) ? second : first;
    }
    return (cur == first) ? second : first;
}

static bool cycle_seat_field(pk_app_t *app, int field, int delta)
{
    if (field == 0) {
        int positions[8];
        int n = effective_seat_positions(&app->caps, positions);
        int cur = find_seat_pos_index(&app->caps, app->draft.seat_position);
        int next = (cur < 0)
                       ? ((delta < 0) ? (n - 1) : 0)
                       : ((cur + ((delta < 0) ? -1 : 1) + n) % n);
        app->draft.seat_position = positions[next];
        return true;
    }
    if (field == 1) {
        int v = app->draft.seat_level;
        if (!cycle_int(&v, 0, 3, 1, delta, v >= 0 && v <= 3)) {
            pk_app_set_msg(app, PK_MSG_SEAT_UNSUPPORTED);
            return false;
        }
        app->draft.seat_level = v;
        return true;
    }
    return false;
}

static bool cycle_editor_field(pk_app_t *app, int delta)
{
    int v;

    if (!app || delta == 0) {
        return false;
    }
    switch (app->edit_key) {
    case PK_SETTING_POWER:
        v = (int)app->draft.power;
        if (!cycle_int(&v, (int)PK_POWER_COMFORT, (int)PK_POWER_ENDURANCE, 1, delta, true)) {
            return false;
        }
        app->draft.power = (pk_power_profile_t)v;
        return true;
    case PK_SETTING_BRIGHTNESS:
        v = app->draft.brightness;
        if (!cycle_int(&v, PK_BRIGHTNESS_MIN, PK_BRIGHTNESS_MAX, PK_BRIGHTNESS_STEP,
                       delta, v >= PK_BRIGHTNESS_MIN && v <= PK_BRIGHTNESS_MAX)) {
            return false;
        }
        app->draft.brightness = v;
        if (app->hal.backlight && app->hal.backlight->set_percent) {
            (void)app->hal.backlight->set_percent(app->hal.backlight_ctx, v);
        }
        return true;
    case PK_SETTING_STEERING:
        app->draft.bool_enabled = (pk_cap_t)cycle_pair_val(
            (int)app->draft.bool_enabled, PK_CAP_NO, PK_CAP_YES, delta);
        return true;
    case PK_SETTING_SEAT:
        return cycle_seat_field(app, app->editor_index, delta);
    default:
        pk_app_set_msg(app, PK_MSG_EDITOR_RANGE);
        return false;
    }
}

void pk_app_begin_editor(pk_app_t *app, pk_setting_id_t key)
{
    if (!app) {
        return;
    }
    app->edit_key = key;
    app->edit_open = true;
    memset(&app->draft, 0, sizeof(app->draft));
    app->draft.key = key;
    app->draft.valid = true;
    app->draft.seat_position = -1;
    app->draft.seat_level = -1;
    app->draft.bool_enabled = PK_CAP_UNKNOWN;
    /* Local rows must keep the current value. A memset-zero draft would
     * silently rewrite endurance (enum 0 = comfort) on a no-edit save.
     */
    app->draft.power = app->power;
    app->draft.brightness = app->brightness;
    app->draft.vehicle = app->vehicle;
    app->draft.connection = app->connection;
    app->draft.session = app->session;
    app->editor_index = 0;
    app->editing = (editor_field_count(key) == 1);
    app->page = PK_PAGE_SETTINGS;
}

void pk_app_cancel_editor(pk_app_t *app)
{
    if (!app) {
        return;
    }
    if (app->edit_open && app->edit_key == PK_SETTING_BRIGHTNESS &&
        app->hal.backlight && app->hal.backlight->set_percent) {
        (void)app->hal.backlight->set_percent(app->hal.backlight_ctx, app->brightness);
    }
    app->has_recheck = false;
    memset(&app->draft, 0, sizeof(app->draft));
    app->edit_open = false;
    app->page = PK_PAGE_SETTINGS;
    pk_arbiter_invalidate(&app->keys);
}

void pk_app_settings_arrow(pk_app_t *app, int delta)
{
    if (!app) {
        return;
    }
    if (!app->edit_open) {
        int n = (int)(sizeof(k_basic_settings) / sizeof(k_basic_settings[0]));
        int idx = 0;
        for (int i = 0; i < n; i++) {
            if (k_basic_settings[i] == app->focus) {
                idx = i;
                break;
            }
        }
        idx = (idx + delta) % n;
        if (idx < 0) {
            idx += n;
        }
        app->focus = k_basic_settings[idx];
        return;
    }
    if (!app->editing) {
        /* Fields + save + cancel. Index wraps. Field counts vary by row. */
        int n = editor_item_count(app->edit_key);
        app->editor_index = (app->editor_index + delta) % n;
        if (app->editor_index < 0) {
            app->editor_index += n;
        }
        return;
    }
    (void)cycle_editor_field(app, delta);
}

bool pk_app_editor_select(pk_app_t *app)
{
    /* Simplified same-page editor: index 0..fields-1 are fields, then save, then cancel. */
    int field_count = 1;
    int item;

    if (!app || !app->edit_open) {
        return false;
    }
    switch (app->edit_key) {
    case PK_SETTING_SEAT:
        field_count = 2;
        break;
    default:
        field_count = 1;
        break;
    }
    item = app->editor_index;
    if (item == field_count + 1) {
        pk_app_cancel_editor(app);
        return false;
    }
    if (item == field_count) {
        return pk_app_save_editor(app);
    }
    if (app->editing) {
        app->editing = false;
        if (field_count == 1) {
            app->editor_index = field_count;
        }
        return true;
    }
    app->editing = true;
    return true;
}

static bool save_local_setting(pk_app_t *app, pk_setting_id_t key)
{
    if (key == PK_SETTING_BRIGHTNESS) {
        int p = app->draft.brightness ? app->draft.brightness : app->brightness;
        if (p < PK_BRIGHTNESS_MIN || p > PK_BRIGHTNESS_MAX || (p % PK_BRIGHTNESS_STEP) != 0) {
            return false;
        }
        app->brightness = p;
        app->store.live.brightness = p;
        persist_live_store(app);
        if (app->hal.backlight && app->hal.backlight->set_percent) {
            (void)app->hal.backlight->set_percent(app->hal.backlight_ctx, p);
        }
        app->page = PK_PAGE_SETTINGS;
        app->edit_open = false;
        return true;
    }
    app->power = app->draft.power;
    app->store.live.power = app->power;
    persist_live_store(app);
    /* Must not drop a protected driving BLE link. */
    app->edit_open = false;
    return true;
}

static bool validate_remote_setting(pk_app_t *app, pk_setting_id_t key)
{
    switch (key) {
    case PK_SETTING_SEAT:
        if (app->draft.seat_position < 0 || app->draft.seat_level < 0 ||
            app->draft.seat_level > 3 ||
            find_seat_pos_index(&app->caps, app->draft.seat_position) < 0) {
            pk_app_set_msg(app, PK_MSG_SEAT_UNSUPPORTED);
            return false;
        }
        return true;
    case PK_SETTING_STEERING:
        if (app->draft.bool_enabled == PK_CAP_UNKNOWN) {
            pk_app_set_msg(app, PK_MSG_EDITOR_INCOMPLETE);
            return false;
        }
        return true;
    default:
        return true;
    }
}

static bool toggle_port_setting(pk_app_t *app)
{
    pk_action_t action;
    pk_intent_t intent;

    if (app->caps.port_open == PK_CAP_YES) {
        action = PK_ACTION_CHARGE_PORT_CLOSE;
    } else if (app->caps.port_open == PK_CAP_NO) {
        action = PK_ACTION_CHARGE_PORT_OPEN;
    } else {
        /* No invented default: port position must come from a real read.
         * The settings entry already runs a silent capability probe; if the
         * car still has not reported it, give the generic failure, not a
         * misleading "asleep" instruction. */
        pk_app_set_msg(app, PK_MSG_UNQUALIFIED);
        return false;
    }
    /* Let pk_app_commit be the single gate: it can start the one-shot
     * qualification read and replay the frozen intent after P is proven. */
    intent = pk_app_make_intent(app, action);
    return pk_app_commit(app, &intent, app->now);
}

/* STITCH C07: one-shot ClimateOn (no on/off toggle, no climate+defog macro). */
static bool activate_climate_on_setting(pk_app_t *app)
{
    pk_intent_t intent;

    app->draft.bool_enabled = PK_CAP_YES;
    intent = pk_app_make_intent(app, PK_ACTION_CLIMATE_ON);
    intent.bool_enabled = PK_CAP_YES;
    return pk_app_commit(app, &intent, app->now);
}

/* STITCH C07: one-shot independent SetPreconditioningMax(true, true)
 * (does not also send ClimateOn). */
static bool activate_defog_setting(pk_app_t *app)
{
    pk_intent_t intent;

    app->draft.bool_enabled = PK_CAP_YES;
    intent = pk_app_make_intent(app, PK_ACTION_PRECONDITION_MAX_ON);
    intent.bool_enabled = PK_CAP_YES;
    return pk_app_commit(app, &intent, app->now);
}

static bool toggle_sentry_setting(pk_app_t *app)
{
    pk_intent_t intent;

    if (!app->caps.sentry_known) {
        pk_app_set_msg(app, PK_MSG_UNQUALIFIED);
        return false;
    }
    /* Direct toggle from the last vehicle snapshot; no draft/confirm page. */
    app->draft.bool_enabled = app->caps.sentry_armed ? PK_CAP_NO : PK_CAP_YES;
    /* Same single gate: a stale/absent park grant must not turn this into a
     * separate user qualification step. */
    intent = pk_app_make_intent(app, PK_ACTION_SET_SENTRY);
    intent.bool_enabled = app->draft.bool_enabled;
    return pk_app_commit(app, &intent, app->now);
}

bool pk_app_save_editor(pk_app_t *app)
{
    pk_setting_id_t key;
    pk_intent_t intent;
    pk_action_t action;

    if (!app || !app->edit_open) {
        return false;
    }
    key = app->edit_key;
    if (key == PK_SETTING_BRIGHTNESS || key == PK_SETTING_POWER) {
        return save_local_setting(app, key);
    }
    if (!validate_remote_setting(app, key)) {
        return false;
    }
    action = pk_setting_action(key);
    intent = pk_app_make_intent(app, action);
    intent.seat_position = app->draft.seat_position;
    intent.seat_level = app->draft.seat_level;
    intent.bool_enabled = app->draft.bool_enabled;
    return pk_app_commit(app, &intent, app->now);
}

bool pk_app_activate_setting(pk_app_t *app, pk_setting_id_t key)
{
    pk_action_t act;
    pk_intent_t intent;

    if (!app) {
        return false;
    }
    app->focus = key;
    if (key == PK_SETTING_POWER) {
        app->power = (app->power == PK_POWER_COMFORT) ? PK_POWER_ENDURANCE : PK_POWER_COMFORT;
        persist_live_store(app);
        return true;
    }
    if (key == PK_SETTING_BRIGHTNESS) {
        pk_app_begin_editor(app, key);
        return true;
    }
    if (key == PK_SETTING_LOGS) {
        app->page = PK_PAGE_LOGS;
        app->log_focus = 0;
        return true;
    }
    if (key == PK_SETTING_HOTSPOT) {
        app->lock_fsm = PK_LOCK_FSM_IDLE;
        pk_app_present_modal(app, PK_PAGE_RESULT);
        pk_app_set_msg(app, PK_MSG_AP_VIN_ONLY);
        return false;
    }
    if (key == PK_SETTING_RESET) {
        if (!pk_app_require_mgmt(app, PK_MGMT_RESET, true)) {
            return false;
        }
        app->reset_focus = 0; /* default cancel */
        app->page = PK_PAGE_RESET_CONFIRM;
        return true;
    }
    if (app->vehicle == PK_VEHICLE_NONE) {
        pk_app_set_msg(app, PK_MSG_LOCAL_ONLY);
        return false;
    }
    if (key == PK_SETTING_SENTRY) {
        return toggle_sentry_setting(app);
    }
    if (key == PK_SETTING_PORT) {
        return toggle_port_setting(app);
    }
    if (key == PK_SETTING_CLIMATE) {
        return activate_climate_on_setting(app);
    }
    if (key == PK_SETTING_DEFOG) {
        return activate_defog_setting(app);
    }
    if (key == PK_SETTING_SEAT || key == PK_SETTING_STEERING) {
        pk_app_begin_editor(app, key);
        return true;
    }
    act = pk_setting_action(key);
    intent = pk_app_make_intent(app, act);
    return pk_app_commit(app, &intent, app->now);
}

bool pk_app_confirm_reset(pk_app_t *app, bool confirmed)
{
    if (!app) {
        return false;
    }
    if (!confirmed) {
        app->page = PK_PAGE_SETTINGS;
        pk_arbiter_invalidate(&app->keys);
        return false;
    }
    if (!pk_app_require_mgmt(app, PK_MGMT_RESET, true)) {
        return false;
    }
    return pk_app_delete_vehicle(app, app->vehicle, true, PK_STORE_CRASH_NONE);
}
