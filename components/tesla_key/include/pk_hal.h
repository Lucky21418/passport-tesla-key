#ifndef PK_HAL_H
#define PK_HAL_H

#include "pk_types.h"
#include "pk_scope.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware / SDK port table. A NULL function pointer must be treated as
 * PK_ERR_NOT_WIRED, never as a successful vehicle or storage result.
 *
 * Button and BLE adapters (pk_btn_adapter, pk_ble_adapter) bind to the
 * application layer. Their ADC / radio hooks may still be NULL.
 *
 * 待真机 items remain open even after a port table is attached.
 */

typedef struct {
    pk_scope_t scope;
    pk_domain_t domain;
    pk_id_t txn_id;
    pk_ms_t deadline;      /* hard cut-off; must not be extended by mid-packets */
    pk_action_t action;
    uint8_t detail;        /* per-action context (e.g. status section index) */
    uint8_t payload[128];
    size_t payload_len;
} pk_ble_frame_t;

typedef struct {
    /*
     * Send exactly once. The transport MUST disable or bypass SDK-built-in
     * retries in Vehicle.Send, StartSession (protocol.ShouldRetry /
     * RetryInterval), and getVCSECResult. Those loops must not become the
     * implementation of one-shot command / enrollment / verify / GET_STATUS
     * / WAKE. Cloud / Fleet wakeup is forbidden. No infinite handshake.
     */
    pk_err_t (*send_once)(void *ctx, const pk_ble_frame_t *frame);
    pk_err_t (*cancel)(void *ctx, pk_id_t txn_id);
    /* Open or close a connection. Must not replay historical vehicle commands. */
    pk_err_t (*connect_once)(void *ctx, int vehicle_slot, pk_ms_t scan_budget_ms);
    pk_err_t (*disconnect)(void *ctx, int vehicle_slot);
    /* Current ATT MTU if known. false means the planner uses its fallback. */
    bool (*get_mtu)(void *ctx, uint16_t *out);
} pk_ble_transport_t;

typedef struct {
    /* prepare candidate image; commit atomically with generation / tombstone. */
    pk_err_t (*prepare)(void *ctx, const void *image, size_t len);
    pk_err_t (*commit)(void *ctx);
    pk_err_t (*recover)(void *ctx, void *image, size_t len);
    /* Must not wipe unrelated NVS namespaces on init failure. */
} pk_nvs_port_t;

typedef struct {
    pk_err_t (*start)(void *ctx, pk_ms_t deadline);
    pk_err_t (*stop)(void *ctx, const char *reason);
    /* After any submit, also wipe the submit buffer. */
    void (*wipe_submit_buffer)(void *ctx);
} pk_softap_port_t;

typedef struct {
    /* CW2017 on shared I2C0, address 0x63. Do not create a second bus.
     * A failed or missing reading is unknown — never fabricated as 0%.
     * No percentage safety floor is invented without measured profile.
     * 待真机: cell profile, pack voltage, lock floor, 500 mAh endurance.
     */
    pk_err_t (*read)(void *ctx, bool *present, int *percent);
} pk_fuel_port_t;

typedef struct {
    /* Backlight on GPIO21 LEDC PWM. 待真机: perceived brightness. */
    pk_err_t (*set_percent)(void *ctx, int percent);
    pk_err_t (*set_awake)(void *ctx, bool awake);
} pk_backlight_port_t;

typedef struct {
    const pk_ble_transport_t *ble;
    void *ble_ctx;
    const pk_nvs_port_t *nvs;
    void *nvs_ctx;
    const pk_softap_port_t *softap;
    void *softap_ctx;
    const pk_fuel_port_t *fuel;
    void *fuel_ctx;
    const pk_backlight_port_t *backlight;
    void *backlight_ctx;
} pk_hal_t;

void pk_hal_init_unwired(pk_hal_t *hal);

#ifdef __cplusplus
}
#endif

#endif /* PK_HAL_H */
