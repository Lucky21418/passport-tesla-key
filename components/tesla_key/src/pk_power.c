#include "pk_app_internal.h"
#include "pk_power.h"

#include <string.h>

/*
 * M12: power, 30 s sleep, reconnect, 0-car.
 *
 * First complete key cycle after the screen is off only wakes, regardless
 * of hold length or which key (including a function key after a power wake).
 * Both power profiles keep a protected driving BLE link. Low battery
 * suppresses the hotspot, decorative refresh, and extra scans, keeps LOCK
 * capability and a readable drive screen, and may deep sleep after screen-off
 * unless a protected keepalive is live. Unknown battery is "—", never "0%".
 * No percentage safety floor is invented without a measured CW2017 profile.
 *
 * Auto-reconnect runs one unexpected-drop round, or a user-started manual
 * round. Comfort: up to 3 × 3 s scans, gaps 2 / 5 s, total ≤ 20 s.
 * Endurance: up to 2 × 3 s, gap 5 s, total ≤ 15 s. Cancel / expiry /
 * attempt cap / switch / low battery / entering VIN all stop it, and
 * never replay a historical vehicle command.
 *
 * 0 cars: no vehicle command, read, or auto-connect. Add, local brightness,
 * power profile, logs, and read-only hotspot info stay reachable.
 * Do not hard-code a default Model 3 as the actual target. A full list
 * of 3 hides Add.
 *
 * 待真机: CW2017 profile, pack voltage, lock floor, 500 mAh endurance,
 * backlight PWM, light/deep sleep vs GPIO0 wake, reconnect energy,
 * unexpected-drop detection.
 */

static const pk_power_policy_t k_policy[PK_POWER_ROLE_COUNT] = {
    [PK_POWER_ROLE_ACTIVE] = {
        "active", false, PK_SCREEN_MS, false
    },
    [PK_POWER_ROLE_CONNECTED_IDLE] = {
        "connected_idle", false, PK_SCREEN_MS, false
    },
    [PK_POWER_ROLE_SCAN] = {
        "scan", false, PK_SCREEN_MS, false
    },
    [PK_POWER_ROLE_DRIVE_KEEPALIVE] = {
        "drive_keepalive", false, PK_SCREEN_MS, false
    },
    [PK_POWER_ROLE_POCKET] = {
        "pocket", true, PK_SCREEN_MS, false
    },
    [PK_POWER_ROLE_LOW_BATT] = {
        "low_batt", true, PK_LOW_BATT_SCREEN_MS, true
    },
    [PK_POWER_ROLE_DESKTOP] = {
        "desktop", false, PK_SCREEN_MS, false
    },
};

/* Product initials, not measured currents. */
static const uint32_t k_role_ua[PK_POWER_ROLE_COUNT] = {
    25000u, /* active: screen + MCU + BLE */
    3000u,  /* connected idle */
    18000u, /* scan */
    6000u,  /* drive keepalive */
    50u,    /* pocket deep sleep */
    100u,   /* low battery, mostly asleep */
    30000u, /* desktop / provisioning */
};

pk_power_role_t pk_power_role(const pk_app_t *app)
{
    if (!app) {
        return PK_POWER_ROLE_POCKET;
    }
    if (app->low_battery) {
        /* Outranks the profile and drive/scan roles for screen/backlight, but
         * should_deep_sleep still vetoes sleep while keepalive is live. */
        return PK_POWER_ROLE_LOW_BATT;
    }
    if (app->moving) {
        return PK_POWER_ROLE_DRIVE_KEEPALIVE;
    }
    if (app->desktop) {
        return PK_POWER_ROLE_DESKTOP;
    }
    if (app->ap) {
        /* AP/provisioning also presents as a desk session; USB host presence
         * is tracked separately in app->desktop. */
        return PK_POWER_ROLE_DESKTOP;
    }
    if (app->reconnect.active) {
        return PK_POWER_ROLE_SCAN;
    }
    if (app->awake) {
        return PK_POWER_ROLE_ACTIVE;
    }
    if (app->power == PK_POWER_COMFORT) {
        /* Comfort keeps the MCU responsive with the screen off; the link may
         * be down, but this profile intentionally avoids deep sleep. */
        return PK_POWER_ROLE_CONNECTED_IDLE;
    }
    return PK_POWER_ROLE_POCKET;
}

const pk_power_policy_t *pk_power_policy(pk_power_role_t role)
{
    if ((unsigned)role >= (unsigned)PK_POWER_ROLE_COUNT) {
        return NULL;
    }
    return &k_policy[role];
}

static pk_power_role_t display_role(const pk_app_t *app)
{
    /* Driving/keepalive must stay readable and must not inherit LOW_BATT's
     * 5 s screen or minimum backlight. */
    if (app && app->moving) {
        return PK_POWER_ROLE_DRIVE_KEEPALIVE;
    }
    if (app && app->desktop) {
        return PK_POWER_ROLE_DESKTOP;
    }
    return pk_power_role(app);
}

pk_ms_t pk_power_screen_timeout(const pk_app_t *app)
{
    const pk_power_policy_t *policy = pk_power_policy(display_role(app));
    return policy ? policy->screen_timeout_ms : PK_SCREEN_MS;
}

uint8_t pk_power_backlight_percent(const pk_app_t *app)
{
    const pk_power_policy_t *policy;

    if (!app) {
        return 0u;
    }
    if (app->lock_alert_active) {
        return 100u;
    }
    policy = pk_power_policy(display_role(app));
    if (policy && policy->min_backlight) {
        return PK_BRIGHTNESS_MIN;
    }
    return (uint8_t)app->brightness;
}

bool pk_power_should_deep_sleep(const pk_app_t *app)
{
    const pk_power_policy_t *policy;

    if (!app || app->awake) {
        return false;
    }
    policy = pk_power_policy(pk_power_role(app));
    if (!policy || !policy->allow_deep_sleep) {
        return false;
    }
    /* In-flight work outranks every power-saving role, even when LOW_BATT
     * wins the role priority. */
    if (app->moving) {
        return false;
    }
    if (app->desktop || app->ap || app->page == PK_PAGE_VIN ||
        app->page == PK_PAGE_PAIR || app->pair_radio_held ||
        app->has_pending || app->reconnect.active ||
        app->lock_alert_active) {
        return false;
    }
    return true;
}

void pk_power_low_battery_enter(pk_app_t *app)
{
    if (!app) {
        return;
    }
    /* Fixed order: stop the hotspot, stop extra scans, then let the LOW_BATT
     * policy shorten the screen and lower the backlight. LOCK and a protected
     * driving link stay untouched. */
    pk_app_close_ap(app, "low-battery");
    pk_app_cancel_reconnect(app);
    pk_app_set_msg(app, PK_MSG_LOW_BATT);
}

uint32_t pk_power_role_current_ua(pk_power_role_t role)
{
    if ((unsigned)role >= (unsigned)PK_POWER_ROLE_COUNT) {
        return 0u;
    }
    return k_role_ua[role];
}

bool pk_power_simulate(const pk_power_phase_t *phases, size_t count,
                       uint16_t capacity_mah, pk_power_estimate_t *out)
{
    uint64_t ua_ms = 0u;
    uint64_t total_ms = 0u;
    uint64_t avg_ua;
    uint64_t days_x100;
    size_t i;

    if (!phases || count == 0u || capacity_mah == 0u || !out) {
        return false;
    }
    for (i = 0; i < count; i++) {
        if ((unsigned)phases[i].role >= (unsigned)PK_POWER_ROLE_COUNT ||
            phases[i].dwell_ms == 0u) {
            return false;
        }
        ua_ms += (uint64_t)pk_power_role_current_ua(phases[i].role) *
                 (uint64_t)phases[i].dwell_ms;
        total_ms += (uint64_t)phases[i].dwell_ms;
    }
    if (total_ms == 0u) {
        return false;
    }
    avg_ua = ua_ms / total_ms;
    if (avg_ua == 0u) {
        avg_ua = 1u;
    }
    days_x100 = (uint64_t)capacity_mah * 100000u / (avg_ua * 24u);
    out->avg_ua = (uint32_t)avg_ua;
    out->days_x100 = (uint32_t)days_x100;
    return true;
}

bool pk_power_common_car_estimate(uint16_t capacity_mah, pk_power_estimate_t *out)
{
    /* 20 interactions × 30 s, 10 scans × 10 s, 8 h drive keepalive,
     * remaining 15 h 48 m 20 s in pocket. Total exactly 24 h. */
    static const pk_power_phase_t k_common_car[] = {
        { PK_POWER_ROLE_ACTIVE, 600000u },
        { PK_POWER_ROLE_SCAN, 100000u },
        { PK_POWER_ROLE_DRIVE_KEEPALIVE, 28800000u },
        { PK_POWER_ROLE_POCKET, 56900000u },
    };
    return pk_power_simulate(k_common_car,
                             sizeof(k_common_car) / sizeof(k_common_car[0]),
                             capacity_mah, out);
}

bool pk_app_start_reconnect(pk_app_t *app, bool manual)
{
    if (!app) {
        return false;
    }
    if (app->vehicle == PK_VEHICLE_NONE) {
        pk_app_set_msg(app, PK_MSG_RECONNECT_NO_CAR);
        return false;
    }
    if (app->ap || app->page == PK_PAGE_PAIR || app->pair_radio_held) {
        pk_app_set_msg(app, PK_MSG_RECONNECT_IN_VIN);
        return false;
    }
    if (app->connected) {
        pk_app_set_msg(app, PK_MSG_ALREADY_CONNECTED);
        return false;
    }
    if (!manual && app->low_battery) {
        return false;
    }
    if (app->reconnect.active) {
        pk_app_set_msg(app, PK_MSG_RECONNECT_RUNNING);
        return false;
    }
    pk_app_invalidate(app, PK_MSG_RECONNECT_NO_REPLAY);
    if (manual) {
        app->page = PK_PAGE_OFFLINE;
    }
    app->reconnect.active = true;
    app->reconnect.attempt = 0;
    if (app->low_battery) {
        app->reconnect.max_attempts = 1;
        app->reconnect.deadline = app->now + PK_RECONNECT_ENDURANCE_TOTAL_MS;
    } else if (app->power == PK_POWER_COMFORT) {
        app->reconnect.max_attempts = PK_RECONNECT_COMFORT_MAX;
        app->reconnect.deadline = app->now + PK_RECONNECT_COMFORT_TOTAL_MS;
    } else {
        app->reconnect.max_attempts = PK_RECONNECT_ENDURANCE_MAX;
        app->reconnect.deadline = app->now + PK_RECONNECT_ENDURANCE_TOTAL_MS;
    }
    app->reconnect.next = app->now;
    return true;
}

bool pk_app_reconnect_attempt(pk_app_t *app, bool result)
{
    pk_reconnect_t *r;
    if (!app) {
        return false;
    }
    r = &app->reconnect;
    if (!r->active || pk_time_before(app->now, r->next)) {
        return false;
    }
    if (pk_time_due(app->now, r->deadline) || r->attempt >= r->max_attempts) {
        r->active = false;
        return false;
    }
    r->attempt++;
    pk_app_log(app, PK_LOG_EVENT_CONNECT, PK_LOG_ERR_NONE);
    if (app->hal.ble && app->hal.ble->connect_once) {
        (void)app->hal.ble->connect_once(app->hal.ble_ctx, app->vehicle, PK_RECONNECT_SCAN_MS);
    }
    if (result) {
        /* Success does not confer qualification. */
        pk_app_set_connected(app, true);
        r->active = false;
        app->page = PK_PAGE_MAIN;
        pk_app_set_msg(app, PK_MSG_RECONNECTED);
        return true;
    }
    if (r->attempt >= r->max_attempts) {
        r->active = false;
        pk_app_set_msg(app, PK_MSG_RECONNECT_STOPPED);
        return false;
    }
    if (app->power == PK_POWER_COMFORT && r->attempt == 1) {
        r->next = app->now + PK_RECONNECT_SCAN_MS + PK_RECONNECT_COMFORT_GAP1_MS;
    } else {
        r->next = app->now + PK_RECONNECT_SCAN_MS +
                  (app->power == PK_POWER_COMFORT ? PK_RECONNECT_COMFORT_GAP2_MS
                                                  : PK_RECONNECT_ENDURANCE_GAP_MS);
    }
    return false;
}

void pk_app_cancel_reconnect(pk_app_t *app)
{
    if (!app) {
        return;
    }
    app->reconnect.active = false;
    pk_arbiter_invalidate(&app->keys);
    pk_app_set_msg(app, PK_MSG_RECONNECT_CANCELED);
}

bool pk_app_should_deep_sleep(const pk_app_t *app)
{
    return pk_power_should_deep_sleep(app);
}
