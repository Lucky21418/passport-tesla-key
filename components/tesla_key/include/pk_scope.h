#ifndef PK_SCOPE_H
#define PK_SCOPE_H

#include "pk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M2: volatile scope for in-flight matching. Never a process-wide sticky flag. */

typedef struct {
    int vehicle;           /* slot index, or PK_VEHICLE_NONE */
    uint32_t connection;
    uint32_t session;
    uint32_t generation;
} pk_scope_t;

typedef struct {
    pk_scope_t scope;
    pk_id_t read_id;
    pk_ms_t requested;
    pk_ms_t received;
    bool sample_present;
    int64_t sample;        /* vehicle-side timestamp as mapped by a verified adapter, else unused */
    bool shift_present;
    bool speed_present;
    bool locked_present;
    pk_ms_t until;
    bool parked;
    bool locked_known;
    bool locked;
    pk_ms_t lock_until;
    bool proof;
} pk_grant_t;

typedef struct {
    pk_action_t action;
    int target;            /* frozen vehicle slot at key-down */
    pk_scope_t scope;      /* frozen at key-down */
    /* Small parameter block; unused fields stay 0 / UNSET. */
    int amps;
    int seat_position;     /* -1 = unset */
    int seat_level;        /* -1 = unset */
    pk_cap_t bool_enabled; /* for sentry / steering: UNKNOWN = unset */
} pk_intent_t;

void pk_scope_clear(pk_scope_t *s);
bool pk_scope_equal(const pk_scope_t *a, const pk_scope_t *b);

/* Match in-flight work: vehicle + domain connection/session + generation
 * + txn as required by the caller. A packet from "the same car over BLE"
 * is not enough. Late, duplicate, or post-timeout packets must not update
 * a new vehicle, a new connection, or a new transaction.
 */
bool pk_scope_same_vehicle_conn_session(const pk_scope_t *a, const pk_scope_t *b);
bool pk_scope_full_match(const pk_scope_t *a, const pk_scope_t *b);

void pk_intent_clear(pk_intent_t *i);
void pk_grant_clear(pk_grant_t *g);

#ifdef __cplusplus
}
#endif

#endif /* PK_SCOPE_H */
