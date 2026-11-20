#ifndef PK_PLAN_H
#define PK_PLAN_H

#include "pk_protocol.h"
#include "pk_risk.h"

/*
 * Declarative request planning.
 *
 * A request class declares its deadline, request-size cap and expected
 * response-size cap. Paging and deadlines come from this table instead of
 * being re-decided in status/command/lock/qualify/pair code.
 *
 * Response caps are product-budget estimates, not measured BLE results. The
 * vehicle's fault 25 (RESPONSE_MTU_EXCEEDED) remains the final arbiter and
 * makes the status page skip that section instead of retrying it. Request
 * caps are enforced before the single BLE write; non-status response caps are
 * advisory (the local side cannot know a reply size before it arrives).
 */

#define PK_PLAN_MTU_FALLBACK  247u
#define PK_PLAN_MTU_HEADER    3u

typedef enum {
    PK_PLAN_STATUS_SECTION = 0,
    PK_PLAN_QUALIFY,
    PK_PLAN_WAKE_THEN_READ,
    PK_PLAN_COMMAND,
    PK_PLAN_LOCK_VERIFY,
    PK_PLAN_UNLOCK_VERIFY,
    PK_PLAN_PAIR_VERIFY,
    PK_PLAN_COUNT
} pk_plan_kind_t;

typedef struct {
    pk_plan_kind_t kind;
    pk_ms_t        deadline_ms;
    uint16_t       max_request_bytes;
    uint16_t       max_response_bytes;
} pk_plan_t;

const pk_plan_t *pk_plan_get(pk_plan_kind_t kind);
/* Request class for an action; budgets are enforced before a BLE write. */
const pk_plan_t *pk_plan_for_action(pk_action_t action);
bool pk_plan_request_fits(pk_action_t action, size_t len);
pk_plan_t pk_plan_status_section(int section);
pk_ms_t pk_plan_deadline(pk_plan_kind_t kind);

/* Request and response payload must both fit one ATT payload (MTU - 3). */
bool pk_plan_fits_mtu(const pk_plan_t *plan, uint16_t mtu);
bool pk_plan_status_section_fits(int section, uint16_t mtu);

/* Bounded replay is derived from the S0 risk table: safe actions only. */
bool pk_plan_replay_allowed(pk_action_t action);

#endif /* PK_PLAN_H */
