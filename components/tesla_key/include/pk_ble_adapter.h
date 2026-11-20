#ifndef PK_BLE_ADAPTER_H
#define PK_BLE_ADAPTER_H

#include "pk_app.h"
#include "pk_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M3 BLE adapter: one-shot transport bound to pk_ble_transport_t.
 *
 * This adapter MUST NOT call or wrap the official Tesla vehicle-command
 * retry loops:
 *   - Vehicle.Send: "retries until vehicle responds with a terminal
 *     result or the context expires", gated by protocol.ShouldRetry and
 *     dispatcher.RetryInterval()
 *   - Vehicle.StartSession: same ShouldRetry / RetryInterval loop
 *   - getVCSECResult: same loop around getReceiver + readUntil
 *   - BLE Connection.RetryInterval() is 1 second
 *
 * A later radio hook is invoked at most once per user intent. There is
 * no retry counter, no 1 s resend, and no infinite handshake. A queued
 * or written frame is not a vehicle accept and not a lock confirmation.
 *
 * Vehicle.Wakeup() uses FleetAPIConnector.Wakeup when that interface is
 * present. This project has no cloud. Wake is allowed only as a single
 * BLE RKE_ACTION_WAKE_VEHICLE (wakeupRKE), and only when the frame
 * action is PK_ACTION_VCSEC_WAKE_ONCE (already gated by M4).
 *
 * If the radio is unattached, or VCSEC / Infotainment session evidence
 * is missing, send_once / connect_once return PK_ERR_NOT_WIRED. That is
 * not a fabricated success, reject, or "vehicle did not execute".
 *
 * 待真机: retry-disabled delivery on the 2023 refreshed Model 3 and
 * rental 3/Y, MTU / segmentation, session rebuild, BLE+SoftAP
 * coexistence, routing-address correlation. Do not advertise zero
 * failure or zero energy.
 */

typedef struct {
    /* Lower-level radio. All may be NULL (default). */
    pk_err_t (*write_once)(void *ctx, const pk_ble_frame_t *frame);
    pk_err_t (*cancel)(void *ctx, pk_id_t txn_id);
    pk_err_t (*connect_once)(void *ctx, int vehicle_slot, pk_ms_t scan_budget_ms);
    pk_err_t (*disconnect)(void *ctx, int vehicle_slot);
    void *radio_ctx;

    /* Session presence. Missing or NULL hook => PK_ERR_NOT_WIRED on send.
     * Do not call SDK StartSession to "fix" a missing session here.
     */
    bool (*has_session)(void *ctx, int vehicle_slot, pk_domain_t domain);
    void *session_ctx;

    /* Clock for deadline checks when the app watermark is not passed in.
     * Optional; send uses the frame deadline only as a cap.
     */
    bool (*now_ms)(void *ctx, pk_ms_t *out);
    void *now_ctx;

    /* Current ATT MTU if the radio knows it. Optional; false => fallback. */
    bool (*get_mtu)(void *ctx, uint16_t *out);
} pk_ble_radio_t;

typedef struct {
    pk_ble_radio_t radio;
    bool in_flight;
    pk_id_t in_flight_txn;
    pk_domain_t in_flight_domain;
    pk_ms_t in_flight_deadline;
    pk_action_t in_flight_action;
    int connected_slot;     /* PK_VEHICLE_NONE if down */
    unsigned sent_once;
    unsigned rejected_unwired;
    unsigned rejected_busy;
    unsigned rejected_wake_policy;
    pk_tx_outcome_t last_tx_outcome;
} pk_ble_adapter_t;

void pk_ble_adapter_init(pk_ble_adapter_t *ad);

/* Function table suitable for pk_hal_t.ble. ctx must be pk_ble_adapter_t *. */
const pk_ble_transport_t *pk_ble_adapter_transport(void);

/* Fill app->hal.ble / ble_ctx only; other HAL ports are left untouched. */
void pk_ble_adapter_attach(pk_app_t *app, pk_ble_adapter_t *ad);

#ifdef __cplusplus
}
#endif

#endif /* PK_BLE_ADAPTER_H */
