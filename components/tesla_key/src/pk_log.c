#include "pk_log.h"

#include <string.h>

void pk_log_init(pk_log_t *log)
{
    pk_log_clear(log);
}

void pk_log_clear(pk_log_t *log)
{
    if (!log) {
        return;
    }
    memset(log, 0, sizeof(*log));
}

static pk_log_event_t sanitize_event(pk_log_event_t event)
{
    switch (event) {
    case PK_LOG_EVENT_COMMAND_INTENT:
    case PK_LOG_EVENT_QUERY:
    case PK_LOG_EVENT_PAIR:
    case PK_LOG_EVENT_CONNECT:
    case PK_LOG_EVENT_DELETE:
    case PK_LOG_EVENT_RESET:
    case PK_LOG_EVENT_POWER:
        return event;
    default:
        return PK_LOG_EVENT_UNKNOWN;
    }
}

static pk_log_err_t sanitize_error(pk_log_err_t error)
{
    switch (error) {
    case PK_LOG_ERR_NONE:
    case PK_LOG_ERR_TIMEOUT:
    case PK_LOG_ERR_REJECTED:
    case PK_LOG_ERR_UNSUPPORTED:
    case PK_LOG_ERR_OFFLINE:
    case PK_LOG_ERR_UNKNOWN:
    case PK_LOG_ERR_CANCELED:
        return error;
    default:
        return PK_LOG_ERR_UNKNOWN;
    }
}

void pk_log_append(pk_log_t *log, pk_log_event_t event, bool slot_current,
                   pk_ms_t elapsed_ms, pk_log_err_t error)
{
    pk_log_record_t *r;

    if (!log) {
        return;
    }
    /* Never write VIN, key material, PIN, MAC, raw frames, or location. */
    r = &log->rec[log->head];
    r->event = sanitize_event(event);
    r->slot_current = slot_current;
    r->elapsed_ms = elapsed_ms;
    r->error = sanitize_error(error);
    log->head = (log->head + 1u) % PK_LOG_CAPACITY;
    if (log->count < PK_LOG_CAPACITY) {
        log->count++;
    }
}

unsigned pk_log_count(const pk_log_t *log)
{
    return log ? log->count : 0;
}

bool pk_log_get(const pk_log_t *log, unsigned index, pk_log_record_t *out)
{
    unsigned first;

    if (!log || !out || index >= log->count) {
        return false;
    }
    first = (log->count == PK_LOG_CAPACITY) ? log->head : 0;
    *out = log->rec[(first + index) % PK_LOG_CAPACITY];
    return true;
}
