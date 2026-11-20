#include "pk_ble_adapter.h"

#include <string.h>

/*
 * One-shot BLE transport. Radio hooks default to NULL, so every send or
 * connect returns PK_ERR_NOT_WIRED and invents no vehicle result.
 *
 * Forbidden wrappers (official snapshot, not linked here):
 *   tesla/pkg/vehicle/vehicle.go  Send, StartSession, Wakeup
 *   tesla/pkg/vehicle/vcsec.go    getVCSECResult, wakeupRKE
 *   tesla/pkg/connector/ble/ble.go RetryInterval() == 1s
 */

static bool deadline_passed(const pk_ble_adapter_t *ad, const pk_ble_frame_t *frame)
{
    pk_ms_t now;
    if (!ad->radio.now_ms || !ad->radio.now_ms(ad->radio.now_ctx, &now)) {
        return false; /* no clock: the app layer still enforces the pending deadline */
    }
    return pk_time_due(now, frame->deadline);
}

static bool session_present(const pk_ble_adapter_t *ad, int vehicle, pk_domain_t domain)
{
    if (!ad->radio.has_session) {
        return false;
    }
    if (vehicle == PK_VEHICLE_NONE) {
        return false;
    }
    return ad->radio.has_session(ad->radio.session_ctx, vehicle, domain);
}

static pk_err_t send_once(void *ctx, const pk_ble_frame_t *frame)
{
    pk_ble_adapter_t *ad = (pk_ble_adapter_t *)ctx;
    pk_err_t rc;

    if (!ad || !frame) {
        return PK_ERR_INVALID;
    }

    /* Cloud / Fleet wakeup is forbidden. The official Wakeup() would pick
     * FleetAPIConnector.Wakeup when that interface is present. This
     * adapter never holds such a connector. A wake frame must be the
     * BLE RKE path only.
     */
    if (frame->action == PK_ACTION_VCSEC_WAKE_ONCE) {
        if (frame->domain != PK_DOMAIN_VCSEC) {
            ad->rejected_wake_policy++;
            return PK_ERR_INVALID;
        }
    }

    if (!ad->radio.write_once) {
        ad->rejected_unwired++;
        ad->last_tx_outcome = PK_TX_NOT_SENT;
        return PK_ERR_NOT_WIRED;
    }
    if (ad->connected_slot == PK_VEHICLE_NONE ||
        frame->scope.vehicle == PK_VEHICLE_NONE ||
        frame->scope.vehicle != ad->connected_slot) {
        ad->rejected_unwired++;
        ad->last_tx_outcome = PK_TX_NOT_SENT;
        return PK_ERR_NOT_WIRED;
    }
    if (!session_present(ad, frame->scope.vehicle, frame->domain)) {
        /* Missing session: do not enter Vehicle.StartSession and its
         * ShouldRetry loop. Report unwired / no session, invent nothing.
         */
        ad->rejected_unwired++;
        ad->last_tx_outcome = PK_TX_NOT_SENT;
        return PK_ERR_NOT_WIRED;
    }
    if (deadline_passed(ad, frame)) {
        ad->last_tx_outcome = PK_TX_NOT_SENT;
        return PK_ERR_TIMEOUT;
    }
    if (ad->in_flight && ad->in_flight_txn != frame->txn_id) {
        /* Same VCSEC domain cannot carry two user intents. The app layer
         * also serializes this; the adapter refuses a second write.
         */
        if (ad->in_flight_domain == frame->domain || frame->domain == PK_DOMAIN_VCSEC) {
            ad->rejected_busy++;
            ad->last_tx_outcome = PK_TX_NOT_SENT;
            return PK_ERR_BUSY;
        }
    }

    ad->in_flight = true;
    ad->in_flight_txn = frame->txn_id;
    ad->in_flight_domain = frame->domain;
    ad->in_flight_deadline = frame->deadline;
    ad->in_flight_action = frame->action;

    /* Exactly one write. No ShouldRetry loop, no RetryInterval resend. */
    rc = ad->radio.write_once(ad->radio.radio_ctx, frame);
    ad->sent_once++;
    if (rc == PK_OK) {
        ad->last_tx_outcome = PK_TX_DELIVERED;
    } else if (rc == PK_ERR_UNKNOWN) {
        ad->last_tx_outcome = PK_TX_SENT_UNKNOWN;
        ad->in_flight = false;
    } else {
        ad->last_tx_outcome = PK_TX_NOT_SENT;
        if (rc == PK_ERR_NOT_WIRED) {
            ad->rejected_unwired++;
        }
        ad->in_flight = false;
    }
    /* A PK_OK here only means the radio accepted the single write.
     * Terminal vehicle results arrive later through the app injectors
     * and must still match scope + txn. Do not map write-OK to accepted.
     */
    return rc;
}

static pk_err_t cancel_once(void *ctx, pk_id_t txn_id)
{
    pk_ble_adapter_t *ad = (pk_ble_adapter_t *)ctx;
    pk_err_t rc = PK_OK;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    if (ad->in_flight && (txn_id == 0 || txn_id == ad->in_flight_txn)) {
        if (ad->radio.cancel) {
            rc = ad->radio.cancel(ad->radio.radio_ctx, txn_id ? txn_id : ad->in_flight_txn);
        } else {
            rc = PK_ERR_NOT_WIRED;
        }
        ad->in_flight = false;
        return (ad->radio.cancel) ? rc : PK_OK;
    }
    if (!ad->radio.cancel) {
        return PK_ERR_NOT_WIRED;
    }
    return ad->radio.cancel(ad->radio.radio_ctx, txn_id);
}

static pk_err_t connect_once(void *ctx, int vehicle_slot, pk_ms_t scan_budget_ms)
{
    pk_ble_adapter_t *ad = (pk_ble_adapter_t *)ctx;
    pk_err_t rc;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    if (vehicle_slot == PK_VEHICLE_NONE) {
        /* First-pair must use PK_VEHICLE_PAIR_CANDIDATE, never invent a
         * store slot or treat NONE as a successful connection.
         */
        return PK_ERR_INVALID;
    }
    if (!ad->radio.connect_once) {
        ad->rejected_unwired++;
        return PK_ERR_NOT_WIRED;
    }
    /* One bounded scan. Must not replay a historical vehicle command. */
    rc = ad->radio.connect_once(ad->radio.radio_ctx, vehicle_slot, scan_budget_ms);
    if (rc == PK_OK) {
        ad->connected_slot = vehicle_slot;
        /* Connection is not qualification, not a session, and not an
         * accept. Session evidence still has to come from has_session.
         */
    }
    return rc;
}

static pk_err_t disconnect_slot(void *ctx, int vehicle_slot)
{
    pk_ble_adapter_t *ad = (pk_ble_adapter_t *)ctx;
    pk_err_t rc = PK_OK;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    ad->in_flight = false;
    if (ad->radio.disconnect) {
        rc = ad->radio.disconnect(ad->radio.radio_ctx, vehicle_slot);
    } else if (ad->connected_slot == PK_VEHICLE_NONE) {
        rc = PK_ERR_NOT_WIRED;
    }
    if (vehicle_slot == ad->connected_slot || vehicle_slot == PK_VEHICLE_NONE) {
        ad->connected_slot = PK_VEHICLE_NONE;
    }
    return rc;
}

static bool adapter_get_mtu(void *ctx, uint16_t *out)
{
    pk_ble_adapter_t *ad = (pk_ble_adapter_t *)ctx;

    if (!ad || !out || !ad->radio.get_mtu) {
        return false;
    }
    return ad->radio.get_mtu(ad->radio.radio_ctx, out);
}

static const pk_ble_transport_t k_transport = {
    .send_once = send_once,
    .cancel = cancel_once,
    .connect_once = connect_once,
    .disconnect = disconnect_slot,
    .get_mtu = adapter_get_mtu,
};

void pk_ble_adapter_init(pk_ble_adapter_t *ad)
{
    if (!ad) {
        return;
    }
    memset(ad, 0, sizeof(*ad));
    ad->connected_slot = PK_VEHICLE_NONE;
}

const pk_ble_transport_t *pk_ble_adapter_transport(void)
{
    return &k_transport;
}

void pk_ble_adapter_attach(pk_app_t *app, pk_ble_adapter_t *ad)
{
    if (!app) {
        return;
    }
    app->hal.ble = pk_ble_adapter_transport();
    app->hal.ble_ctx = ad;
    /* Radio hooks stay NULL until a later physical / stack port is bound.
     * Sends then return PK_ERR_NOT_WIRED and invent no success.
     */
}
