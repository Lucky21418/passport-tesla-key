#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_risk.h"

#include <ctype.h>
#include <string.h>

bool pk_action_is_park_gated(pk_action_t a)
{
    /* Single source: the risk contract decides, not this file. */
    return pk_risk_park_gated(pk_action_risk(a));
}

bool pk_setting_is_local(pk_setting_id_t k)
{
    return k == PK_SETTING_POWER || k == PK_SETTING_BRIGHTNESS ||
           k == PK_SETTING_LOGS || k == PK_SETTING_HOTSPOT || k == PK_SETTING_RESET;
}

pk_action_t pk_setting_action(pk_setting_id_t k)
{
    switch (k) {
    case PK_SETTING_CLIMATE:
        return PK_ACTION_CLIMATE_ON;
    case PK_SETTING_DEFOG:
        return PK_ACTION_PRECONDITION_MAX_ON;
    case PK_SETTING_SENTRY:
        return PK_ACTION_SET_SENTRY;
    case PK_SETTING_VENT:
        return PK_ACTION_WINDOW_VENT;
    case PK_SETTING_FLASH:
        return PK_ACTION_FLASH;
    case PK_SETTING_HORN:
        return PK_ACTION_HONK;
    case PK_SETTING_SEAT:
        return PK_ACTION_SET_SEAT_HEAT;
    case PK_SETTING_STEERING:
        return PK_ACTION_SET_STEERING_HEAT;
    default:
        return PK_ACTION_NONE;
    }
}

void pk_app_caps_clear(pk_caps_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    /* clock_verified and adapter_verified remain false. Do not put
     * synthetic timebase fixtures in the production path (C12).
     */
}

bool pk_vin_valid(const char *text, char out[PK_VIN_BUF])
{
    char buf[PK_VIN_BUF];
    size_t n = 0;
    size_t i;
    const unsigned char *p;

    if (!text || !out) {
        return false;
    }
    p = (const unsigned char *)text;
    while (*p && n < PK_VIN_LEN) {
        if (isspace(*p)) {
            p++;
            continue;
        }
        buf[n++] = (char)toupper(*p++);
    }
    while (*p) {
        if (!isspace(*p)) {
            return false; /* longer than 17 after trim */
        }
        p++;
    }
    buf[n] = '\0';
    if (n != PK_VIN_LEN) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = buf[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
        if (!ok) {
            return false;
        }
    }
    /* Regional check-digit is enabled only for a confirmed market; never
     * imposed globally as a North-American check.
     */
    memcpy(out, buf, PK_VIN_BUF);
    return true;
}

pk_scope_t pk_app_scope(const pk_app_t *app)
{
    pk_scope_t s;
    pk_scope_clear(&s);
    if (!app) {
        return s;
    }
    s.vehicle = app->vehicle;
    s.connection = app->connection;
    s.session = app->session;
    s.generation = app->generation;
    return s;
}

pk_tx_outcome_t pk_app_note_send(pk_app_t *app, pk_err_t rc)
{
    pk_tx_outcome_t outcome = PK_TX_NOT_SENT;

    if (!app) {
        return PK_TX_NOT_SENT;
    }
    if (app->hal.ble == pk_ble_adapter_transport() && app->hal.ble_ctx) {
        outcome = ((pk_ble_adapter_t *)app->hal.ble_ctx)->last_tx_outcome;
    } else if (rc == PK_OK) {
        outcome = (app->last_tx_outcome == PK_TX_DELIVERED) ? PK_TX_DELIVERED
                                                             : PK_TX_NOT_SENT;
    } else if (rc == PK_ERR_UNKNOWN || app->last_tx_outcome == PK_TX_SENT_UNKNOWN) {
        outcome = PK_TX_SENT_UNKNOWN;
    }
    if (rc == PK_ERR_UNKNOWN) {
        outcome = PK_TX_SENT_UNKNOWN;
    } else if (rc != PK_OK && outcome != PK_TX_SENT_UNKNOWN) {
        outcome = PK_TX_NOT_SENT;
    }
    app->last_tx_outcome = outcome;
    return outcome;
}

void pk_app_motion_unknown(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->moving = false;
    app->motion = PK_MOTION_UNKNOWN;
    app->motion_until = 0;
}

bool pk_app_known_motion(const pk_app_t *app)
{
    if (!app || !app->moving) {
        return false;
    }
    if (app->motion == PK_MOTION_TENDENCY && app->motion_until != 0 &&
        pk_time_due(app->now, app->motion_until)) {
        return false;
    }
    return true;
}

bool pk_app_parked(const pk_app_t *app)
{
    const pk_grant_t *g;
    pk_scope_t cur;

    if (!app || !app->has_grant || pk_app_known_motion(app)) {
        return false;
    }
    g = &app->grant;
    cur = pk_app_scope(app);
    return g->proof &&
           pk_scope_full_match(&g->scope, &cur) &&
           app->now < g->until &&
           g->parked;
}

bool pk_app_park_fresh(const pk_app_t *app)
{
    if (!pk_app_parked(app)) {
        return false;
    }
    return !pk_time_elapsed(app->now, app->grant.received, PK_PARK_FRESH_MS);
}

bool pk_app_can_vcsec_unlock(const pk_app_t *app)
{
    /* D07: explicit VCSEC unlock is allowed unless motion is a known
     * tendency. Asleep/locked is not the predicate and is not a park proof.
     * Motion unknown is an accepted risk, not a parked claim. */
    if (!app || !app->connected || app->vehicle == PK_VEHICLE_NONE) {
        return false;
    }
    if (pk_app_known_motion(app)) {
        return false;
    }
    return true;
}

pk_intent_t pk_app_make_intent(pk_app_t *app, pk_action_t action)
{
    pk_intent_t i;
    pk_intent_clear(&i);
    if (!app) {
        return i;
    }
    i.action = action;
    i.target = app->vehicle;
    i.scope = pk_app_scope(app);
    return i;
}

pk_action_t pk_app_primary(const pk_app_t *app)
{
    if (!app || app->vehicle == PK_VEHICLE_NONE) {
        return PK_ACTION_NO_TARGET;
    }
    if (pk_app_known_motion(app)) {
        return PK_ACTION_LOCK;
    }
    /* Unknown or unconfirmed lock state MUST NOT default to LOCK: return
     * read-only VCSEC status query so pressing ● reconciles real state. */
    if (!app->vcsec_status_valid || app->vcsec_lock_state == PK_LOCK_READ_UNKNOWN) {
        return PK_ACTION_VCSEC_GET_STATUS_ONCE;
    }
    if (app->vcsec_lock_state == PK_LOCK_READ_LOCKED ||
        app->vcsec_lock_state == PK_LOCK_READ_INTERNAL_LOCKED ||
        app->vcsec_lock_state == PK_LOCK_READ_SELECTIVE_UNLOCKED) {
        return PK_ACTION_UNLOCK;
    }
    return PK_ACTION_LOCK;
}

const pk_offline_body_spec_t *pk_app_offline_body_spec(void)
{
    static const pk_offline_body_spec_t k_spec = {
        .top = {
            .action = PK_ACTION_FLASH,
            .title = "寻车闪灯",
            .gesture = "单击 ▲",
            .status_hint = "请先重连",
            .enabled = false,
        },
        .mid = {
            .action = PK_ACTION_RECONNECT,
            .title = "重新连接",
            .gesture = "单击 ●",
            .status_hint = "发起有界扫描连接",
            .enabled = true,
        },
        .bot = {
            .action = PK_ACTION_HONK,
            .title = "寻车鸣笛",
            .gesture = "长按 ▼",
            .status_hint = "请先重连",
            .enabled = false,
        },
    };
    return &k_spec;
}
