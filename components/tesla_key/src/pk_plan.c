#include "pk_plan.h"

static const pk_plan_t k_plans[PK_PLAN_COUNT] = {
    [PK_PLAN_STATUS_SECTION] = {
        PK_PLAN_STATUS_SECTION, PK_STATUS_READ_MS, 64u, 200u
    },
    [PK_PLAN_QUALIFY] = {
        PK_PLAN_QUALIFY, PK_QUALIFICATION_READ_MS, 64u, 200u
    },
    [PK_PLAN_WAKE_THEN_READ] = {
        PK_PLAN_WAKE_THEN_READ, PK_WAKE_THEN_READ_MS, 32u, 200u
    },
    [PK_PLAN_COMMAND] = {
        PK_PLAN_COMMAND,
        PK_COMMAND_WAIT_MS,
        128u /* pk_ble_frame_t.payload */,
        (uint16_t)(PK_PLAN_MTU_FALLBACK - PK_PLAN_MTU_HEADER)
    },
    [PK_PLAN_LOCK_VERIFY] = {
        PK_PLAN_LOCK_VERIFY, PK_LOCK_VERIFY_MS, 64u,
        (uint16_t)(PK_PLAN_MTU_FALLBACK - PK_PLAN_MTU_HEADER)
    },
    [PK_PLAN_UNLOCK_VERIFY] = {
        PK_PLAN_UNLOCK_VERIFY, PK_LOCK_VERIFY_MS, 64u,
        (uint16_t)(PK_PLAN_MTU_FALLBACK - PK_PLAN_MTU_HEADER)
    },
    [PK_PLAN_PAIR_VERIFY] = {
        PK_PLAN_PAIR_VERIFY, PK_PAIR_VERIFY_MS,
        128u /* pk_ble_frame_t.payload */,
        (uint16_t)(PK_PLAN_MTU_FALLBACK - PK_PLAN_MTU_HEADER)
    },
};

/* One VehicleData section per status request. These are conservative product
 * budgets used for MTU paging; fault 25 is still the vehicle's final answer. */
static const uint16_t k_status_response_max[PK_STATUS_SECTION_COUNT] = {
    160u, /* charge */
    160u, /* climate */
    200u, /* drive */
    120u, /* closures */
    120u, /* tires */
};

const pk_plan_t *pk_plan_get(pk_plan_kind_t kind)
{
    if ((unsigned)kind >= (unsigned)PK_PLAN_COUNT) {
        return NULL;
    }
    return &k_plans[kind];
}

const pk_plan_t *pk_plan_for_action(pk_action_t action)
{
    switch (action) {
    case PK_ACTION_STATUS_READ:
        return &k_plans[PK_PLAN_STATUS_SECTION];
    case PK_ACTION_GET_QUALIFICATION:
        return &k_plans[PK_PLAN_QUALIFY];
    case PK_ACTION_VCSEC_WAKE_ONCE:
        return &k_plans[PK_PLAN_WAKE_THEN_READ];
    case PK_ACTION_PAIR_VERIFY_VCSEC:
    case PK_ACTION_PAIR_VERIFY_INFOTAINMENT:
        return &k_plans[PK_PLAN_PAIR_VERIFY];
    case PK_ACTION_VCSEC_GET_STATUS_ONCE:
        return &k_plans[PK_PLAN_LOCK_VERIFY];
    default:
        return &k_plans[PK_PLAN_COMMAND];
    }
}

bool pk_plan_request_fits(pk_action_t action, size_t len)
{
    const pk_plan_t *plan = pk_plan_for_action(action);
    return plan && len <= (size_t)plan->max_request_bytes;
}

pk_plan_t pk_plan_status_section(int section)
{
    pk_plan_t plan = k_plans[PK_PLAN_STATUS_SECTION];

    if (section < 0 || section >= PK_STATUS_SECTION_COUNT) {
        plan.max_request_bytes = 0u;
        plan.max_response_bytes = 0u;
        return plan;
    }
    plan.max_response_bytes = k_status_response_max[section];
    return plan;
}

pk_ms_t pk_plan_deadline(pk_plan_kind_t kind)
{
    const pk_plan_t *plan = pk_plan_get(kind);
    return plan ? plan->deadline_ms : 0u;
}

bool pk_plan_fits_mtu(const pk_plan_t *plan, uint16_t mtu)
{
    if (!plan || mtu <= PK_PLAN_MTU_HEADER) {
        return false;
    }
    if (plan->max_request_bytes == 0u || plan->max_response_bytes == 0u) {
        return false;
    }
    return plan->max_request_bytes + PK_PLAN_MTU_HEADER <= mtu &&
           plan->max_response_bytes + PK_PLAN_MTU_HEADER <= mtu;
}

bool pk_plan_status_section_fits(int section, uint16_t mtu)
{
    pk_plan_t plan = pk_plan_status_section(section);
    return pk_plan_fits_mtu(&plan, mtu);
}

bool pk_plan_replay_allowed(pk_action_t action)
{
    return pk_risk_replay_allowed(pk_action_risk(action));
}
