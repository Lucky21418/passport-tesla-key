#ifndef PORT_BLE_STUB_NIMBLE_H
#define PORT_BLE_STUB_NIMBLE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_HS_CONN_HANDLE_NONE 0xFFFFu
#define BLE_HS_EDONE            14
#define BLE_HS_ENOTCONN         7
#define BLE_OWN_ADDR_PUBLIC     0
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND 3
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND    2

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

typedef struct {
    uint8_t type;
} ble_uuid_t;

typedef struct {
    ble_uuid_t u;
    uint16_t value;
} ble_uuid16_t;

typedef struct {
    ble_uuid_t u;
    uint8_t value[16];
} ble_uuid128_t;

typedef union {
    ble_uuid_t u;
    ble_uuid16_t u16;
    ble_uuid128_t u128;
} ble_uuid_any_t;

#define BLE_UUID16_INIT(v) { .u = { .type = 16 }, .value = (v) }
#define BLE_UUID128_INIT(...) { .u = { .type = 128 }, .value = { __VA_ARGS__ } }

struct os_mbuf {
    const uint8_t *om_data;
    uint16_t om_len;
};

#define OS_MBUF_PKTLEN(om) ((om) ? (om)->om_len : 0u)

struct ble_gatt_error {
    int status;
    uint16_t att_handle;
};

struct ble_gatt_attr {
    uint16_t handle;
};

struct ble_gatt_svc {
    uint16_t start_handle;
    uint16_t end_handle;
};

struct ble_gatt_chr {
    uint16_t def_handle;
    uint16_t val_handle;
    ble_uuid_any_t uuid;
};

struct ble_gatt_dsc {
    uint16_t handle;
    ble_uuid_any_t uuid;
};

struct ble_hs_adv_fields {
    const uint8_t *name;
    uint8_t name_len;
};

struct ble_gap_disc_params {
    uint16_t itvl;
    uint16_t window;
    uint8_t filter_policy;
    uint8_t limited;
    uint8_t passive;
    uint8_t filter_duplicates;
};

struct ble_gap_upd_params {
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
};

struct ble_gap_disc_desc {
    uint8_t event_type;
    uint8_t length_data;
    ble_addr_t addr;
    int8_t rssi;
    const uint8_t *data;
};

enum {
    BLE_GAP_EVENT_CONNECT = 0,
    BLE_GAP_EVENT_DISCONNECT = 1,
    BLE_GAP_EVENT_DISC = 2,
    BLE_GAP_EVENT_DISC_COMPLETE = 3,
    BLE_GAP_EVENT_MTU = 4,
    BLE_GAP_EVENT_NOTIFY_RX = 5
};

struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; } disconnect;
        struct ble_gap_disc_desc disc;
        struct { int reason; } disc_complete;
        struct { uint16_t conn_handle; uint16_t channel_id; uint16_t value; } mtu;
        struct { uint16_t conn_handle; uint16_t attr_handle; struct os_mbuf *om; } notify_rx;
    };
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
typedef int ble_gatt_attr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);
typedef int ble_gatt_mtu_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t mtu, void *arg);
typedef int ble_gatt_disc_svc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service, void *arg);
typedef int ble_gatt_chr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);
typedef int ble_gatt_dsc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

struct ble_hs_cfg_s {
    void (*sync_cb)(void);
};
extern struct ble_hs_cfg_s ble_hs_cfg;

int ble_uuid_cmp(const ble_uuid_t *u1, const ble_uuid_t *u2);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields, const uint8_t *src, uint8_t len);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *data, uint16_t max_len, uint16_t *len);
int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type);
int ble_hs_util_ensure_addr(int prefer_random);
int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_disc_cancel(void);
int ble_gap_disc_active(void);
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                    int32_t duration_ms, const void *conn_params,
                    ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
int ble_gap_update_params(uint16_t conn_handle, const struct ble_gap_upd_params *params);
int ble_gap_conn_rssi(uint16_t conn_handle, int8_t *out_rssi);
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle,
                         const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg);
int ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg);
int ble_gattc_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid,
                               ble_gatt_disc_svc_fn *cb, void *cb_arg);
int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, ble_gatt_chr_fn *cb, void *cb_arg);
int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_val_handle,
                            uint16_t end_handle, ble_gatt_dsc_fn *cb, void *cb_arg);

esp_err_t nimble_port_init(void);
int nimble_port_stop(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);
void nimble_port_freertos_init(void (*host_task)(void *));
void nimble_port_freertos_deinit(void);

#endif
