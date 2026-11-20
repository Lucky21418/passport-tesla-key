#include "pk_app_internal.h"

#include <string.h>

/*
 * M14: navigation and page contract. This phase does not edit official
 * Stitch pages. Later firmware UI must honour: no new menu or find-car
 * page; parked double-● → list; driving double-● → read-only status;
 * wait/result ● only exits local wait; a page return must not let the
 * original press continue as a home-screen command.
 *
 * List: ▲ cycles up; ▼ on a stored car → delete confirm (default cancel);
 * ▼ on another row cycles down; short-● selects / enters VIN / status /
 * settings; long-● home.
 *
 * Visual contract (for a later renderer, not implemented here):
 * Chinese ≥ 12 px, top 22 / bottom 36, exact 240×320, no touch, no blur /
 * filter / animation / remote assets. #E82127 only for unlock / real
 * alerts; cancel / reconnect / power-save are not red. Icons only from
 * the registered CBI / MDI set; do not claim Tesla official licensing.
 *
 * Do not change the three-key home mapping, do not open driving-writable
 * navigation, do not restore the old "ultimate" interaction.
 * t816u discarded main-screen vehicle identity; it is not implemented.
 */

int pk_app_list_items(const pk_app_t *app, int *out, int max)
{
    int n = 0;
    int i;
    if (!app || !out || max <= 0) {
        return 0;
    }
    for (i = 0; i < PK_MAX_VEHICLES && n < max; i++) {
        if (pk_store_slot_used(&app->store, i)) {
            out[n++] = i;
        }
    }
    if (n < PK_MAX_VEHICLES && n < max) {
        out[n++] = PK_LIST_ADD;
    }
    if (n < max) {
        out[n++] = PK_LIST_QUICK;
    }
    if (n < max) {
        out[n++] = PK_LIST_STATUS;
    }
    if (n < max) {
        out[n++] = PK_LIST_SETTINGS;
    }
    return n;
}

void pk_app_list_arrow(pk_app_t *app, pk_key_id_t key)
{
    int items[8];
    int n;
    int i;
    int focus;
    int idx = 0;
    if (!app) {
        return;
    }
    n = pk_app_list_items(app, items, 8);
    if (n <= 0) {
        return;
    }
    focus = app->car_focus;
    for (i = 0; i < n; i++) {
        if (items[i] == focus) {
            idx = i;
            break;
        }
    }
    if (i == n) {
        idx = 0;
        focus = items[0];
    }
    if (key == PK_KEY_DOWN && focus >= 0 && pk_store_slot_used(&app->store, focus)) {
        app->delete_target = focus;
        app->delete_focus = 0; /* default cancel */
        pk_arbiter_invalidate(&app->keys);
        app->page = PK_PAGE_DELETE_CONFIRM;
        return;
    }
    if (key == PK_KEY_UP) {
        idx = (idx == 0) ? n - 1 : idx - 1;
    } else {
        idx = (idx + 1) % n;
    }
    app->car_focus = items[idx];
}

bool pk_app_select_list_focus(pk_app_t *app)
{
    int items[8];
    int n;
    int i;
    int focus;
    if (!app) {
        return false;
    }
    n = pk_app_list_items(app, items, 8);
    focus = app->car_focus;
    for (i = 0; i < n; i++) {
        if (items[i] == focus) {
            break;
        }
    }
    if (i == n && n > 0) {
        focus = items[0];
    }
    if (focus >= 0) {
        return pk_app_select_vehicle(app, focus);
    }
    if (focus == PK_LIST_ADD) {
        return pk_app_enter_vin(app);
    }
    if (focus == PK_LIST_QUICK) {
        app->settings_quick_mode = true;
        app->focus = PK_SETTING_FLASH;
        return pk_app_navigate(app, PK_PAGE_SETTINGS);
    }
    if (focus == PK_LIST_STATUS) {
        pk_app_open_status(app, PK_ORIGIN_LIST);
        return true;
    }
    if (focus == PK_LIST_SETTINGS) {
        app->settings_quick_mode = false;
        app->focus = PK_SETTING_BRIGHTNESS;
        return pk_app_navigate(app, PK_PAGE_SETTINGS);
    }
    return false;
}
