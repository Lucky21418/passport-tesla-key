#ifndef PK_RISK_H
#define PK_RISK_H

#include "pk_types.h"

/*
 * Risk contract: every action has exactly one risk class. Gestures, park gates,
 * replay permission and confirmation are derived from this class instead of
 * being re-decided per screen or per call site.
 *
 *  SAFE      wrong direction is harmless (lock, navigation, status reads).
 *            Not a product-layer replay right. LOCK may be sent explicitly
 *            while moving; it is never auto-resent.
 *  RISKY     opens the car, draws attention, or changes comfort/sentry.
 *            Requires a deliberate gesture; never replayed. D09 decides
 *            which of these are blocked while moving.
 *  DANGEROUS closures and the charge port. Requires a fresh qualified-park
 *            snapshot and a long hold; never replayed.
 */
typedef enum {
    PK_RISK_SAFE = 0,
    PK_RISK_RISKY,
    PK_RISK_DANGEROUS
} pk_risk_t;

pk_risk_t pk_action_risk(pk_action_t action);
bool pk_risk_park_gated(pk_risk_t risk);
bool pk_risk_replay_allowed(pk_risk_t risk);
unsigned pk_risk_hold_ms(pk_risk_t risk);
const char *pk_risk_name(pk_risk_t risk);

/*
 * Hold gesture for a concrete action. Pure UI navigation (select, return,
 * reconnect) is not a vehicle command, so it keeps the base long-press
 * window; every vehicle action takes its hold from the risk class.
 */
unsigned pk_action_hold_ms(pk_action_t action);

/*
 * True when a known motion tendency blocks the action (D09). Shared by the
 * command guard and the UI. Qualified park is a separate gate.
 */
bool pk_action_moving_blocked(pk_action_t action);

#endif /* PK_RISK_H */
