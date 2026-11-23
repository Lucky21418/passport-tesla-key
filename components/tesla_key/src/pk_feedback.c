#include "pk_feedback.h"
#include "pk_copy.h"

static bool is_progress_msg(pk_msg_id_t id)
{
    switch (id) {
    case PK_MSG_PRESSED:
    case PK_MSG_SENT_WAIT:
    case PK_MSG_QUAL_IN_PROGRESS:
    case PK_MSG_LOCK_VERIFYING:
    case PK_MSG_CMD_WAIT_MID:
    case PK_MSG_APPROVED_NEED_AUTH:
    case PK_MSG_RECONNECT_RUNNING:
        return true;
    default:
        return false;
    }
}

static bool is_unverified_msg(pk_msg_id_t id)
{
    switch (id) {
    case PK_MSG_CMD_ACCEPTED:
        return true;
    default:
        return false;
    }
}

static bool is_success_msg(pk_msg_id_t id)
{
    switch (id) {
    case PK_MSG_LOCK_CONFIRMED:
    case PK_MSG_QUAL_OK:
    case PK_MSG_PAIR_SAVED:
    case PK_MSG_VIN_OK:
    case PK_MSG_RECONNECTED:
    case PK_MSG_RECORD_UPDATED:
    case PK_MSG_DELETE_RECOVERED:
        return true;
    default:
        return false;
    }
}

pk_feedback_state_t pk_feedback_state(const pk_app_t *app)
{
    if (!app) {
        return PK_FEEDBACK_IDLE;
    }
    /* Failure is more important than a stale wait surface. */
    if (pk_msg_is_failure(app->message)) {
        return PK_FEEDBACK_FAILURE;
    }
    if (app->page == PK_PAGE_WAIT) {
        return PK_FEEDBACK_PROGRESS;
    }
    if (is_progress_msg(app->message) || (app->has_pending && app->pending.kind != PK_PENDING_NONE)) {
        return PK_FEEDBACK_PROGRESS;
    }
    if (is_unverified_msg(app->message)) {
        return PK_FEEDBACK_UNVERIFIED;
    }
    if (is_success_msg(app->message)) {
        return PK_FEEDBACK_SUCCESS;
    }
    return PK_FEEDBACK_IDLE;
}
