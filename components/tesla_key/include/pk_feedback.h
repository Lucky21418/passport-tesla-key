#ifndef PK_FEEDBACK_H
#define PK_FEEDBACK_H

#include "pk_app.h"

/*
 * Feedback tri-state for the result surface.
 *
 * PROGRESS  waiting on the vehicle; no terminal claim
 * SUCCESS   a state flip or an explicitly accepted/confirmed result
 * FAILURE   rejected / unconfirmed / timeout; red presentation
 *
 * Audio and "zero copy while in progress" are hardware/UX acceptance items;
 * this module only makes the classification single-source.
 */
typedef enum {
    PK_FEEDBACK_IDLE = 0,
    PK_FEEDBACK_PROGRESS,
    PK_FEEDBACK_UNVERIFIED, /* vehicle accepted/queued; state not read back */
    PK_FEEDBACK_SUCCESS,    /* a state flip or locally verified result */
    PK_FEEDBACK_FAILURE
} pk_feedback_state_t;

pk_feedback_state_t pk_feedback_state(const pk_app_t *app);

#endif /* PK_FEEDBACK_H */
