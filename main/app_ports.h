#pragma once

#include "pk_app.h"
#include "pk_ble_adapter.h"
#include "pk_btn_adapter.h"
#include "pk_protocol.h"

/*
 * Hardware & protocol port layer for FoloToy AI Passport (ESP32-C3):
 *   - BSP ADC button ladder (raw mV polling -> pk_btn_adapter)
 *   - BSP LEDC backlight & CW2017 fuel gauge
 *   - NVS atomic store + per-vehicle P-256 keypair persistence
 *   - SoftAP + HTTP portal (PASSPORT-KEY, 192.168.4.1, 120s cap) for VIN entry
 *   - NimBLE Central + GATT Client (0x0211 / 0x0212 / 0x0213) wired to pk_protocol
 */
void app_ports_init(pk_app_t *app, pk_btn_adapter_t *btn, pk_ble_adapter_t *ble);
void app_ports_poll_buttons(pk_btn_adapter_t *btn, pk_ms_t now);
void app_ports_poll_events(pk_app_t *app, pk_ms_t now);
void app_ports_refresh_battery(pk_app_t *app);

/* Retrieve masked VIN tail (e.g. "***3243") for display on vehicle list / pair pages */
void app_ports_format_masked_vin(const char *vin, char *out, size_t out_len);

/* Retrieve latest cached BLE RSSI (dBm) when connected */
bool app_ports_ble_rssi(int8_t *out_rssi);

/* Retrieve candidate pairing BLE state: 0=scanning, 1=connected, 2=addkey sent */
int app_ports_ble_pair_state(const pk_app_t *app);

/* Stop active BLE scanning/reconnect (used by pocket stuck-key protection) */
void app_ports_stop_ble_scan(void);

/* Turn display on/off via DISPON/DISPOFF + backlight */
void app_ports_set_screen_awake(bool awake);

/* Enter board-safe Deep Sleep with GPIO0 low-level wakeup (returns false if key held) */
bool app_ports_enter_deep_sleep(void);
