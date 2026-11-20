#include "pk_app_internal.h"

#include <string.h>

/*
 * M7: independent RemoteDrive and keep-alive bound to the current car
 * and connection/session (t813u). Accept is not "可挂挡". ClosuresState
 * optional_remote_start is not PIN, shift, or remaining-authorization proof.
 *
 * PIN is never collected here.
 *
 * 30 s sleep, low battery, and power-profile changes never drop the
 * link while a command is in flight. BLE being online is not itself a
 * drive grant.
 *
 * Unexpected disconnect / reboot: drive authorization is unconfirmed;
 * it is not restored automatically. Ending local protection requires a
 * fresh P+stationary grant and an explicit user confirmation.
 *
 * The 60 s notice is a local prompt lifetime, not a vehicle-side lease,
 * and is not renewed as a grant.
 *
 * 待真机: in-car keyless setting, PIN rejection, real vehicle-side
 * authorization duration, keep-alive energy and stability in sleep /
 * endurance profiles.
 */

bool pk_app_management_allowed(const pk_app_t *app, pk_mgmt_op_t op, bool current,
                               pk_msg_id_t *why_out)
{
    pk_msg_id_t why = PK_MSG_EMPTY;
    bool ok = true;

    if (!app) {
        if (why_out) {
            *why_out = PK_MSG_NO_TARGET;
        }
        return false;
    }
    /* Do not use parked() as a blanket lock on switch / add / pair. */
    if (pk_app_known_motion(app) || app->busy) {
        why = PK_MSG_MGMT_BUSY_OR_MOVING;
        ok = false;
    } else if (op == PK_MGMT_DELETE && !current) {
        ok = true; /* non-current delete: reachable when not driving and not busy */
    } else if (op == PK_MGMT_SWITCH || op == PK_MGMT_ADD || op == PK_MGMT_PAIR) {
        ok = true; /* reachable while offline or when qualification is unreadable */
    } else if (op == PK_MGMT_DELETE || op == PK_MGMT_RESET) {
        /* Missing park must not seal the local management entry. A known
         * motion tendency already failed the check above. Local delete does
         * not claim the vehicle revoked the key. */
        ok = true;
    }
    if (why_out) {
        *why_out = why;
    }
    return ok;
}

bool pk_app_require_mgmt(pk_app_t *app, pk_mgmt_op_t op, bool current)
{
    pk_msg_id_t why = PK_MSG_EMPTY;
    if (pk_app_management_allowed(app, op, current, &why)) {
        return true;
    }
    if (app && why != PK_MSG_EMPTY) {
        pk_app_set_msg(app, why);
    }
    return false;
}

