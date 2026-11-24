#include "pk_app_internal.h"
#include "pk_risk.h"

#include <string.h>

/*
 * Raw PRESS / RELEASE arbitration on a monotonic watermark.
 *
 * Contract (FoloToy BSP + shipped 1234/1149): 180 ms click/double window,
 * 500 ms long press. Short = act, double = back/status, long = high-consequence
 * action; long never means "back". A single click commits only at t > deadline;
 * a second PRESS at t <= deadline is a double. tick(deadline) never commits.
 *
 * Adapters feed only PK_EDGE_PRESS / PK_EDGE_RELEASE. Cooked BSP single/double/
 * long events are not consumed here. An invalid ADC reading is not a press.
 */

static bool setting_is_dangerous(pk_setting_id_t key)
{
    return key == PK_SETTING_VENT || key == PK_SETTING_PORT ||
           key == PK_SETTING_RESET;
}

static void page_back(pk_app_t *app)
{
    if (app->edit_open) {
        pk_app_cancel_editor(app);
        return;
    }
    switch (app->page) {
    case PK_PAGE_SETTINGS:
        (void)pk_app_navigate(app, PK_PAGE_MAIN);
        break;
    case PK_PAGE_STATUS:
        pk_app_exit_status(app);
        break;
    case PK_PAGE_CARS:
        (void)pk_app_navigate(app, PK_PAGE_MAIN);
        break;
    case PK_PAGE_VIN:
    case PK_PAGE_PAIR:
        pk_app_pair_cancel(app);
        (void)pk_app_navigate(app, PK_PAGE_CARS);
        break;
    case PK_PAGE_DELETE_CONFIRM:
        (void)pk_app_navigate(app, PK_PAGE_CARS);
        break;
    case PK_PAGE_RESET_CONFIRM:
        (void)pk_app_navigate(app, PK_PAGE_SETTINGS);
        break;
    case PK_PAGE_WAIT:
    case PK_PAGE_RESULT:
        pk_app_exit_wait(app);
        break;
    default:
        (void)pk_app_navigate(app, PK_PAGE_MAIN);
        break;
    }
}

static void short_action(pk_app_t *app, const pk_intent_t *intent, pk_ms_t t)
{
    switch (app->page) {
    case PK_PAGE_MAIN:
    case PK_PAGE_OFFLINE:
        if (intent) {
            (void)pk_app_commit(app, intent, t);
        }
        break;
    case PK_PAGE_SETTINGS:
        if (app->edit_open) {
            (void)pk_app_editor_select(app);
        } else if (setting_is_dangerous(app->focus)) {
            /* A short tap must not open a closure or the charge port. */
            pk_app_set_msg(app, PK_MSG_HOLD_DANGER);
        } else {
            (void)pk_app_activate_setting(app, app->focus);
        }
        break;
    case PK_PAGE_STATUS:
        if (app->connected && !app->has_pending) {
            pk_app_open_status(app, app->origin);
        } else {
            pk_app_exit_status(app);
        }
        break;
    case PK_PAGE_WAIT:
    case PK_PAGE_RESULT:
        pk_app_exit_wait(app);
        break;
    default:
        if (intent) {
            (void)pk_app_commit(app, intent, t);
        }
        break;
    }
}

/* Long press is deliberately reserved for high-consequence or mode-changing
 * actions; it never means "back". Back is a double click. */
static void long_action(pk_app_t *app)
{
    if (app->page == PK_PAGE_MAIN) {
        if (pk_app_navigate(app, PK_PAGE_SETTINGS)) {
            app->settings_quick_mode = false;
            app->focus = PK_SETTING_BRIGHTNESS;
        }
        return;
    }
    if (app->page == PK_PAGE_SETTINGS && !app->edit_open &&
        setting_is_dangerous(app->focus)) {
        (void)pk_app_activate_setting(app, app->focus);
        return;
    }
    if (app->page == PK_PAGE_PAIR || app->page == PK_PAGE_VIN) {
        pk_app_pair_cancel(app);
        (void)pk_app_navigate(app, PK_PAGE_CARS);
    }
}

static void double_action(pk_app_t *app)
{
    if (app->page == PK_PAGE_MAIN || app->page == PK_PAGE_OFFLINE) {
        if (!app->connected) {
            (void)pk_app_navigate(app, PK_PAGE_CARS);
            return;
        }
        pk_app_open_status(app, app->moving ? PK_ORIGIN_DRIVING : PK_ORIGIN_LIST);
        return;
    }
    page_back(app);
}

void pk_keys_commit_click_if_due(pk_app_t *app, pk_ms_t t)
{
    pk_intent_t intent;
    pk_arbiter_t *k;

    if (!app) {
        return;
    }
    k = &app->keys;
    if (!k->deadline_valid || k->down || !pk_time_before(k->deadline, t)) {
        return;
    }
    k->deadline_valid = false;
    if (!k->has_intent) {
        if (app->page == PK_PAGE_MAIN && !app->connected &&
            app->vehicle != PK_VEHICLE_NONE) {
            (void)pk_app_start_reconnect(app, true);
        }
        return;
    }
    intent = k->intent;
    k->has_intent = false;
    short_action(app, &intent, t);
}

void pk_keys_dot_press(pk_app_t *app, pk_ms_t t)
{
    pk_arbiter_t *k;

    if (!app) {
        return;
    }
    k = &app->keys;
    if (k->down) {
        return;
    }
    k->second = k->deadline_valid && !pk_time_before(k->deadline, t);
    k->deadline_valid = false;
    k->down = true;
    k->down_at = t;
    k->consumed = false;
    k->exit_cycle = false;
    k->wake_cycle = false;
    app->last_activity = t;

    if (!app->awake || app->consume_as_wake_only) {
        app->awake = true;
        k->consumed = true;
        k->wake_cycle = true;
        if (app->hal.backlight && app->hal.backlight->set_awake) {
            (void)app->hal.backlight->set_awake(app->hal.backlight_ctx, true);
        }
        return;
    }
    pk_app_set_msg(app, PK_MSG_PRESSED);

    if (app->page == PK_PAGE_WAIT || app->page == PK_PAGE_RESULT) {
        k->exit_cycle = true;
        return;
    }
    if (app->page == PK_PAGE_OFFLINE) {
        k->has_intent = true;
        k->intent = pk_app_make_intent(app, PK_ACTION_RECONNECT);
        return;
    }
    if (app->busy) {
        k->consumed = true;
        pk_app_set_msg(app, PK_MSG_BUSY_NO_QUEUE);
        return;
    }
    if (app->page == PK_PAGE_MAIN && !app->connected &&
        app->vehicle != PK_VEHICLE_NONE) {
        k->has_intent = false;
        pk_app_set_msg(app, PK_MSG_RECONNECT_FIRST);
        return;
    }

    k->has_intent = true;
    k->intent = pk_app_make_intent(app, app->page == PK_PAGE_MAIN
                                            ? pk_app_primary(app)
                                            : PK_ACTION_SELECT);
}

void pk_keys_dot_release(pk_app_t *app, pk_ms_t t)
{
    pk_arbiter_t *k;
    pk_ms_t held;

    if (!app) {
        return;
    }
    k = &app->keys;
    app->last_activity = t;
    if (!k->down) {
        return;
    }
    held = t - k->down_at;
    if (k->exit_cycle && !k->consumed) {
        pk_app_exit_wait(app);
    } else if (!k->consumed && k->second) {
        double_action(app);
        k->consumed = true;
    } else if (!k->consumed && held >= PK_MAIN_HOLD_MS) {
        long_action(app);
        k->consumed = true;
    } else if (!k->consumed) {
        k->deadline = t + PK_DOUBLE_MS;
        k->deadline_valid = true;
    }
    k->down = false;
    k->second = false;
    k->exit_cycle = false;
    if (k->wake_cycle) {
        app->consume_as_wake_only = false;
    }
    k->wake_cycle = false;
}

void pk_keys_tick(pk_app_t *app, pk_ms_t t)
{
    pk_arbiter_t *k;

    if (!app) {
        return;
    }
    k = &app->keys;
    if (k->down && !k->consumed && !k->exit_cycle &&
        (t - k->down_at) >= PK_MAIN_HOLD_MS) {
        long_action(app);
        k->consumed = true;
        k->deadline_valid = false;
    }
    pk_keys_commit_click_if_due(app, t);
}

bool pk_side_press(pk_app_t *app, pk_key_id_t key, pk_ms_t t)
{
    pk_action_t action;

    if (!app) {
        return false;
    }
    app->last_activity = t;
    if (!app->awake || app->consume_as_wake_only) {
        app->awake = true;
        app->side.active = true;
        app->side.key = key;
        app->side.at = t;
        app->side.consumed = true;
        if (app->hal.backlight && app->hal.backlight->set_awake) {
            (void)app->hal.backlight->set_awake(app->hal.backlight_ctx, true);
        }
        return false;
    }
    if (app->side.active) {
        return false;
    }
    if (app->page != PK_PAGE_MAIN) {
        app->side.active = true;
        app->side.key = key;
        app->side.at = t;
        app->side.consumed = true;
        if (app->page == PK_PAGE_STATUS) {
            if (key == PK_KEY_UP && !pk_app_known_motion(app)) {
                (void)pk_app_navigate(app, PK_PAGE_CARS);
            }
            return false;
        }
        if (app->page == PK_PAGE_QUALIFICATION_OFFER) {
            if (app->offer_focus == PK_OFFER_CHECK) {
                app->offer_focus = PK_OFFER_CANCEL;
            } else if (app->offer_focus == PK_OFFER_CANCEL) {
                app->offer_focus = PK_OFFER_CHECK;
            } else if (app->offer_focus == PK_OFFER_WAKE) {
                app->offer_focus = PK_OFFER_BACK;
            } else {
                app->offer_focus = PK_OFFER_WAKE;
            }
        } else if (app->page == PK_PAGE_SETTINGS) {
            pk_app_settings_arrow(app, key == PK_KEY_UP ? -1 : 1);
        } else if (app->page == PK_PAGE_CARS) {
            pk_app_list_arrow(app, key);
        } else if (app->page == PK_PAGE_DELETE_CONFIRM) {
            app->delete_focus = app->delete_focus ? 0 : 1;
        } else if (app->page == PK_PAGE_RESET_CONFIRM) {
            app->reset_focus = app->reset_focus ? 0 : 1;
        } else if (app->page == PK_PAGE_LOGS) {
            if (key == PK_KEY_UP && app->log_focus > 0) {
                app->log_focus--;
            } else if (key == PK_KEY_DOWN) {
                app->log_focus++;
            }
        } else if (app->page == PK_PAGE_VIN) {
            if (key == PK_KEY_DOWN) {
                pk_app_pair_cancel(app);
                (void)pk_app_navigate(app, PK_PAGE_CARS);
            }
        } else if (app->page == PK_PAGE_PAIR) {
            if (key == PK_KEY_DOWN) {
                pk_app_pair_cancel(app);
                (void)pk_app_navigate(app, PK_PAGE_CARS);
            } else if (key == PK_KEY_UP && app->pair_step == 2 && !app->enrollment_sent) {
                app->pair_step = 1;
            }
        } else {
            pk_app_set_msg(app, PK_MSG_SIDE_NOT_ON_MAIN);
        }
        return false;
    }

    action = (key == PK_KEY_UP) ? PK_ACTION_FRUNK_OPEN : PK_ACTION_TRUNK_MOVE;
    app->side.active = true;
    app->side.key = key;
    app->side.at = t;
    app->side.intent = pk_app_make_intent(app, action);
    /* The commit gate owns park qualification; holding decides the action, and
     * a missing/stale grant starts one read and replays the frozen intent. */
    app->side.consumed = false;
    pk_app_set_msg(app, key == PK_KEY_UP ? PK_MSG_HOLD_FRUNK : PK_MSG_HOLD_TRUNK);
    return true;
}

void pk_side_release(pk_app_t *app, pk_key_id_t key, pk_ms_t t)
{
    if (!app || !app->side.active || app->side.key != key) {
        return;
    }
    app->last_activity = t;

    if (!app->side.consumed) {
        pk_app_set_msg(app, PK_MSG_CANCELLED);
    }
    /* Once sent, a release cannot withdraw the command. */
    app->side.active = false;
    /* A wake-only ▲/▼ cycle after a power-key wake is now complete. */
    if (app->consume_as_wake_only) {
        app->consume_as_wake_only = false;
    }
}

void pk_side_tick(pk_app_t *app, pk_ms_t t)
{
    if (!app) {
        return;
    }
    if (!app->side.active) {
        return;
    }
    if (app->side.consumed) {
        if (app->page == PK_PAGE_SETTINGS && !app->edit_open &&
            t - app->side.at >= 450u) {
            pk_app_settings_arrow(app, app->side.key == PK_KEY_UP ? -1 : 1);
            app->side.at = t - 270u;
        }
        return;
    }
    if (t - app->side.at >= pk_action_hold_ms(app->side.intent.action)) {
        app->side.consumed = true;
        (void)pk_app_commit(app, &app->side.intent, t);
    }
}

void pk_power_press(pk_app_t *app, pk_ms_t t)
{
    if (!app) {
        return;
    }
    app->last_activity = t;
    /* Product initials: ~0.5 s power-on, ~2 s power-off. 待真机.
     * A power wake followed by a function key is still first-cycle wake-only
     * for that function-key cycle if the screen was off.
     */
    if (!app->awake) {
        app->awake = true;
        app->consume_as_wake_only = true;
        if (app->hal.backlight && app->hal.backlight->set_awake) {
            (void)app->hal.backlight->set_awake(app->hal.backlight_ctx, true);
        }
    }
}

void pk_power_release(pk_app_t *app, pk_ms_t t)
{
    if (!app) {
        return;
    }
    app->last_activity = t;
    /* Off-hold handling is a later power-management concern; not wired. */
}
