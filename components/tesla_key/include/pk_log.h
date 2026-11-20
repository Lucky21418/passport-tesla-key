#ifndef PK_LOG_H
#define PK_LOG_H

#include "pk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M13: RAM ring of 64 desensitized records. No VIN, keys, fingerprints,
 * PIN, MAC, raw frames, routing payloads, tokens, or location.
 * No export / upload. ESP_LOG, UART, and coredump paths must obey the
 * same rule. 待真机: confirm the official protocol stack and BLE host
 * logs do not print those fields by default.
 */

typedef struct {
    pk_log_event_t event;
    bool slot_current;     /* anonymous: current / none */
    pk_ms_t elapsed_ms;    /* relative monotonic watermark */
    pk_log_err_t error;
} pk_log_record_t;

typedef struct {
    pk_log_record_t rec[PK_LOG_CAPACITY];
    unsigned head;         /* next write */
    unsigned count;
} pk_log_t;

void pk_log_init(pk_log_t *log);
void pk_log_clear(pk_log_t *log);

/* Unknown event / error codes are coerced to UNKNOWN. */
void pk_log_append(pk_log_t *log, pk_log_event_t event, bool slot_current,
                   pk_ms_t elapsed_ms, pk_log_err_t error);

unsigned pk_log_count(const pk_log_t *log);
/* index 0 is the oldest retained record. */
bool pk_log_get(const pk_log_t *log, unsigned index, pk_log_record_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PK_LOG_H */
