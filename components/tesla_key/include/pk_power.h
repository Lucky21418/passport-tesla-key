#ifndef PK_POWER_H
#define PK_POWER_H

#include "pk_app.h"

/*
 * Role-based power arbitration.
 *
 * One policy table decides screen timeout, deep-sleep permission and
 * backlight floor for each role. Keepalive/drive protection always outranks a
 * power-saving decision: no role may tear down a protected driving link or a
 * pending command.
 *
 * Current numbers are product initials, not measured currents. The host
 * simulator is a budgeting tool; it is not a battery-life claim.
 */

typedef enum {
    PK_POWER_ROLE_ACTIVE = 0,
    PK_POWER_ROLE_CONNECTED_IDLE,
    PK_POWER_ROLE_SCAN,
    PK_POWER_ROLE_DRIVE_KEEPALIVE,
    PK_POWER_ROLE_POCKET,
    PK_POWER_ROLE_LOW_BATT,
    PK_POWER_ROLE_DESKTOP,
    PK_POWER_ROLE_COUNT
} pk_power_role_t;

typedef struct {
    const char *name;
    bool        allow_deep_sleep;
    pk_ms_t     screen_timeout_ms;
    bool        min_backlight;
} pk_power_policy_t;

typedef struct {
    pk_power_role_t role;
    uint32_t        dwell_ms;
} pk_power_phase_t;

typedef struct {
    uint32_t avg_ua;
    uint32_t days_x100;
} pk_power_estimate_t;

pk_power_role_t pk_power_role(const pk_app_t *app);
const pk_power_policy_t *pk_power_policy(pk_power_role_t role);
pk_ms_t pk_power_screen_timeout(const pk_app_t *app);
uint8_t pk_power_backlight_percent(const pk_app_t *app);
bool pk_power_should_deep_sleep(const pk_app_t *app);

/* Fixed degradation order: close AP, cancel reconnect, fast screen, min
 * backlight, then keep LOCK and a protected driving link. */
void pk_power_low_battery_enter(pk_app_t *app);

uint32_t pk_power_role_current_ua(pk_power_role_t role);

/* Integer-only budget model: average current and days at capacity_mah. */
bool pk_power_simulate(const pk_power_phase_t *phases, size_t count,
                       uint16_t capacity_mah, pk_power_estimate_t *out);
bool pk_power_common_car_estimate(uint16_t capacity_mah, pk_power_estimate_t *out);

#endif /* PK_POWER_H */
