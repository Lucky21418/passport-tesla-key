#include "pk_hal.h"

#include <string.h>

void pk_hal_init_unwired(pk_hal_t *hal)
{
    if (!hal) {
        return;
    }
    memset(hal, 0, sizeof(*hal));
    /* All ports remain NULL. Callers must return PK_ERR_NOT_WIRED and
     * must not invent a successful BLE, NVS, SoftAP, or fuel-gauge result.
     */
}

