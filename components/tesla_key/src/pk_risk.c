#include "pk_risk.h"
#include "pk_policy.h"

pk_risk_t pk_action_risk(pk_action_t action)
{
    switch (action) {
    /* Wrong direction is harmless. */
    case PK_ACTION_LOCK:
    case PK_ACTION_SELECT:
    case PK_ACTION_RETURN_MAIN:
    case PK_ACTION_RECONNECT:
    case PK_ACTION_GET_QUALIFICATION:
    case PK_ACTION_STATUS_READ:
    case PK_ACTION_VCSEC_WAKE_ONCE:
    case PK_ACTION_VCSEC_GET_STATUS_ONCE:
        return PK_RISK_SAFE;

    /* Opens the car or draws attention; deliberate gesture, no replay. */
    case PK_ACTION_UNLOCK:
    case PK_ACTION_FLASH:
    case PK_ACTION_HONK:
    case PK_ACTION_CLIMATE_ON:
    case PK_ACTION_PRECONDITION_MAX_ON:
    /* Comfort settings are legitimate while driving and do not move the car
     * or open a closure. They stay non-replayed and still need a deliberate
     * gesture, but a P proof would be over-defense: it added a DriveState read
     * and could refuse a seat heater at speed. */
    case PK_ACTION_SET_SENTRY:
    case PK_ACTION_SET_SEAT_HEAT:
    case PK_ACTION_SET_STEERING_HEAT:
        return PK_RISK_RISKY;

    /* Closures, motion and the charge port: park evidence + long hold. */
    case PK_ACTION_FRUNK_OPEN:
    case PK_ACTION_TRUNK_MOVE:
    case PK_ACTION_CHARGE_PORT_OPEN:
    case PK_ACTION_CHARGE_PORT_CLOSE:
    case PK_ACTION_WINDOW_VENT:
        return PK_RISK_DANGEROUS;

    default:
        /* Pairing/enrollment and any unknown action get the careful treatment. */
        return PK_RISK_RISKY;
    }
}

bool pk_risk_park_gated(pk_risk_t risk)
{
    return risk == PK_RISK_DANGEROUS;
}

bool pk_risk_replay_allowed(pk_risk_t risk)
{
    /* SAFE marks a low-consequence explicit command (LOCK may be sent while
     * moving). It is not permission to generate a second product intent. */
    (void)risk;
    return false;
}

unsigned pk_risk_hold_ms(pk_risk_t risk)
{
    switch (risk) {
    case PK_RISK_SAFE:
        return 0u;
    case PK_RISK_RISKY:
        return PK_MAIN_HOLD_MS;
    case PK_RISK_DANGEROUS:
        return PK_DANGER_HOLD_MS;
    default:
        return PK_DANGER_HOLD_MS;
    }
}

const char *pk_risk_name(pk_risk_t risk)
{
    switch (risk) {
    case PK_RISK_SAFE:
        return "safe";
    case PK_RISK_RISKY:
        return "risky";
    case PK_RISK_DANGEROUS:
        return "dangerous";
    default:
        return "unknown";
    }
}

unsigned pk_action_hold_ms(pk_action_t action)
{
    switch (action) {
    /* Navigation never reaches the car; keep the base long-press window. */
    case PK_ACTION_SELECT:
    case PK_ACTION_RETURN_MAIN:
    case PK_ACTION_RECONNECT:
        return PK_MAIN_HOLD_MS;
    default:
        return pk_risk_hold_ms(pk_action_risk(action));
    }
}

bool pk_action_moving_blocked(pk_action_t action)
{
    /* D09: known motion blocks unlock, sentry, flash/horn, and closures.
     * Climate, defrost, seat heat, and steering heat stay available.
     * LOCK is explicit-only and is not in this set. Park-gated closures are
     * blocked here as well as by the qualification gate. */
    switch (action) {
    case PK_ACTION_UNLOCK:
    case PK_ACTION_SET_SENTRY:
    case PK_ACTION_FLASH:
    case PK_ACTION_HONK:
        return true;
    default:
        return pk_risk_park_gated(pk_action_risk(action));
    }
}
