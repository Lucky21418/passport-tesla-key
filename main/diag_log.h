#ifndef DIAG_LOG_H
#define DIAG_LOG_H

#include <stdint.h>
#include <stddef.h>

/* Small persistent black box for field tests without a PC.
 * Records are appended in RAM and flushed to NVS from the main task. The next
 * boot prints the previous session over USB Serial/JTAG before clearing it. */

typedef enum {
    DIAG_BOOT = 1,
    DIAG_SCAN = 2,
    DIAG_CONN = 3,
    DIAG_DISC = 4,
    DIAG_SESSION = 5,
    DIAG_SESSION_FAIL = 6,
    DIAG_FAULT = 7,
    DIAG_TX_FAIL = 8,
    DIAG_RX_PLAINTEXT = 9,
    DIAG_SEND = 10,
    DIAG_HINT = 11,
    DIAG_QUAL = 12,  /* qualification reply: action=shift, code=flags */
    DIAG_MTU = 13    /* code=negotiated ATT MTU */
} diag_type_t;

void diag_log_init(void);
void diag_event(uint8_t type, uint8_t domain, uint8_t action, int code, int aux);
/* Keep the last few decoded inner payloads so a field failure can be diagnosed
 * without a live capture. dir: 0=tx, 1=rx. Never stores keys or VINs by design
 * (payload only, and only the last DIAG_RAW_CAP messages). */
void diag_raw(uint8_t dir, uint8_t domain, uint8_t action, const uint8_t *data, size_t len);
void diag_log_tick(void);

#endif
