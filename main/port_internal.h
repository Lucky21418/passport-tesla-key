#ifndef PORT_INTERNAL_H
#define PORT_INTERNAL_H

#include "app_ports.h"
#include "pk_app_internal.h"
#include "pk_hal.h"
#include "pk_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PORT_EV_BLE_CONNECTED = 1,
    PORT_EV_BLE_DISCONNECTED,
    PORT_EV_BLE_RX_FRAME,
    PORT_EV_HTTP_VIN_SUBMIT,
    PORT_EV_BLE_SLOTS_FULL
} port_ev_type_t;

typedef struct {
    port_ev_type_t type;
    int            slot;
    uint32_t       conn_gen;
    uint16_t       len;
    union {
        int8_t     rx_buf_idx;
        char       vin[PK_VIN_BUF];
    } u;
} port_event_t;

/* Shared core & RX buffer pool accessors (implemented in app_ports.c) */
pk_ms_t port_now_ms(void);
bool port_post_event(const port_event_t *ev);
int port_rx_buf_alloc_copy(const uint8_t *data, size_t len);
const uint8_t *port_rx_buf_ptr(int idx);
void port_rx_buf_release(int idx);
pk_app_t *port_bound_app(void);
pk_ble_adapter_t *port_bound_ble(void);

/* NVS persistence & per-slot keys (implemented in port_nvs.c) */
const pk_nvs_port_t *port_nvs_ops(void);
bool port_nvs_available(void);
void port_nvs_load_slot_keys(void);
bool port_ensure_slot_keypair(int slot);
pk_proto_key_slot_t *port_slot_keys(int slot);
const char *port_slot_vin(int slot);

/* SoftAP + HTTP VIN entry portal (implemented in port_wifi_portal.c) */
const pk_softap_port_t *port_softap_ops(void);

/* NimBLE Central + GATT transport (implemented in port_ble.c) */
void port_ble_init_and_attach(pk_app_t *app, pk_ble_adapter_t *ble);
void port_ble_deinit(void);
void port_ble_stop_scan(void);
void port_ble_handle_event(pk_app_t *app, const port_event_t *ev, pk_ms_t now);
void port_ble_poll(pk_app_t *app, pk_ms_t now);
void port_ble_start_candidate_pairing(pk_app_t *app, const char *vin);
bool port_ble_rssi(int8_t *out_rssi);
int port_ble_pair_state(const pk_app_t *app);

#endif
