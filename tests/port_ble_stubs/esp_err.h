#ifndef PORT_BLE_STUB_ESP_ERR_H
#define PORT_BLE_STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL               -1
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_NVS_NOT_FOUND  0x1102

static inline const char *esp_err_to_name(esp_err_t code)
{
    return (code == ESP_OK) ? "ESP_OK" : "ESP_ERR";
}

#endif
