#ifndef PORT_BLE_STUB_NVS_H
#define PORT_BLE_STUB_NVS_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns; (void)mode;
    if (out) *out = 1u;
    return ESP_OK;
}
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h; (void)k; (void)out; (void)len;
    return ESP_ERR_NVS_NOT_FOUND;
}
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *val, size_t len)
{
    (void)h; (void)k; (void)val; (void)len;
    return ESP_OK;
}
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h; (void)k;
    return ESP_OK;
}
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

#endif
