#include "pk_scope.h"

#include <string.h>

void pk_scope_clear(pk_scope_t *s)
{
    if (!s) {
        return;
    }
    s->vehicle = PK_VEHICLE_NONE;
    s->connection = 0;
    s->session = 0;
    s->generation = 0;
}

bool pk_scope_equal(const pk_scope_t *a, const pk_scope_t *b)
{
    return pk_scope_full_match(a, b);
}

bool pk_scope_same_vehicle_conn_session(const pk_scope_t *a, const pk_scope_t *b)
{
    if (!a || !b) {
        return false;
    }
    return a->vehicle == b->vehicle &&
           a->vehicle != PK_VEHICLE_NONE &&
           a->connection == b->connection &&
           a->session == b->session;
}

bool pk_scope_full_match(const pk_scope_t *a, const pk_scope_t *b)
{
    if (!a || !b) {
        return false;
    }
    return a->vehicle == b->vehicle &&
           a->connection == b->connection &&
           a->session == b->session &&
           a->generation == b->generation;
}

void pk_intent_clear(pk_intent_t *i)
{
    if (!i) {
        return;
    }
    memset(i, 0, sizeof(*i));
    i->action = PK_ACTION_NONE;
    i->target = PK_VEHICLE_NONE;
    pk_scope_clear(&i->scope);
    i->amps = 0;
    i->seat_position = -1;
    i->seat_level = -1;
    i->bool_enabled = PK_CAP_UNKNOWN;
}

void pk_grant_clear(pk_grant_t *g)
{
    if (!g) {
        return;
    }
    memset(g, 0, sizeof(*g));
    pk_scope_clear(&g->scope);
}
