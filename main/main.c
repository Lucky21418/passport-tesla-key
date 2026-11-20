#include "app_ports.h"
#include "app_ui.h"
#include "pk_power.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "diag_log.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "pk_app.h"

#if defined(__has_include)
#if __has_include("dev_vin.h")
#include "dev_vin.h"
#define HAS_DEV_VIN 1
#endif
#endif
#ifndef HAS_DEV_VIN
#define HAS_DEV_VIN 0
#endif

static const char *TAG = "tesla_key";

/* No PSRAM: total free heap is not enough, the largest contiguous block is
 * what can still host a Wi-Fi/BLE allocation. Log both at the few points where
 * the memory map changes so a future regression is visible in the boot log. */
static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static pk_app_t s_app;
static pk_btn_adapter_t s_btn;
static pk_ble_adapter_t s_ble;
static key_guard_t s_key_guard;
static key_stuck_guard_t s_stuck_guard;

static pk_ms_t now_ms(void)
{
    return (pk_ms_t)(esp_timer_get_time() / 1000);
}

static void poll_key_wake_guard(void)
{
    if (!s_key_guard.armed) {
        return;
    }
    const int mv = bsp_button_read_mv();
    const bool released = (mv >= PK_BTN_MV_RELEASED_MIN);
    if (key_guard_consume(&s_key_guard, released, esp_timer_get_time()) &&
        !s_key_guard.armed) {
        ESP_LOGI(TAG, "唤醒键已松手，按键恢复响应");
    }
}

static uint8_t effective_backlight_percent(const pk_app_t *app)
{
    return pk_power_backlight_percent(app);
}

void app_main(void)
{
    esp_err_t err;
    bool was_awake = true;
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();

    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒：先屏蔽按键动作，直到唤醒键松手");
    }

    {
        const esp_pm_config_t pm_cfg = {
            .max_freq_mhz = 160,
            .min_freq_mhz = 40,
            .light_sleep_enable = false,
        };
        const esp_err_t pm_err = esp_pm_configure(&pm_cfg);
        if (pm_err != ESP_OK) {
            ESP_LOGW(TAG, "DFS PM configure failed (%s)", esp_err_to_name(pm_err));
        }
    }

    err = nvs_flash_init();
    diag_log_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Do not wipe unrelated namespaces on a soft failure. */
        ESP_LOGW(TAG, "NVS init failed (%s); store remains unwired", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "LVGL init failed");
        return;
    }

    if (bsp_button_init(NULL, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button ADC init failed; poll will return NOT_WIRED");
    } else if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= PK_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }

    pk_app_init(&s_app, PK_VEHICLE_NONE, false);
    log_heap("before ports");
    app_ports_init(&s_app, &s_btn, &s_ble);
    log_heap("after BLE init");
#if HAS_DEV_VIN
    /* Development preset: pre-fill the VIN so a fresh device skips the Wi-Fi
     * VIN entry step and can pair directly. Only applied when no VIN is set
     * and the store is not full. Value lives in git-ignored main/dev_vin.h. */
    if (!s_app.has_vin && pk_store_count(&s_app.store) < PK_MAX_VEHICLES) {
        char dev_vin[PK_VIN_BUF];
        if (pk_vin_valid(DEV_VIN, dev_vin)) {
            memcpy(s_app.vin, dev_vin, PK_VIN_BUF);
            s_app.has_vin = true;
            ESP_LOGI(TAG, "dev VIN preset applied");
        }
    }
#endif
    bool was_desktop = usb_serial_jtag_is_connected();
    pk_app_set_desktop(&s_app, was_desktop);

    bsp_lvgl_lock(-1);
    app_ui_init();
    app_ui_update(&s_app);
    lv_refr_now(NULL);
    bsp_lvgl_unlock();
    bsp_display_backlight(effective_backlight_percent(&s_app));

    ESP_LOGI(TAG, "特斯拉蓝牙随身钥匙已启动");

    pk_ms_t last_batt_ms = now_ms();
    bool was_low_batt = s_app.low_battery;

    for (;;) {
        const pk_ms_t now = now_ms();
        const int mv = bsp_button_read_mv();
        const bool key_pressed = (mv >= 0 && mv < PK_BTN_MV_RELEASED_MIN);
        const key_stuck_action_t stuck_act =
            key_stuck_update(&s_stuck_guard, key_pressed, now, PK_STUCK_KEY_MS);

        const unsigned stuck_policy = key_stuck_loop_policy(stuck_act);

        if (stuck_act == KEY_STUCK_ENTER) {
            ESP_LOGW(TAG, "GPIO0 连续按压超过 10s，判定为兜内卡键：停扫灭屏等松手");
            pk_app_cancel_reconnect(&s_app);
            app_ports_stop_ble_scan();
            app_ports_set_screen_awake(false);
        }

        poll_key_wake_guard();
        if ((stuck_policy & KEY_STUCK_POLICY_SUPPRESS_NEW) == 0 && !s_key_guard.armed) {
            (void)pk_btn_adapter_feed_mv(&s_btn, now, mv, mv >= 0);
        }
        app_ports_poll_events(&s_app, now);
        (void)pk_app_tick(&s_app, now);
        diag_log_tick();

        if ((stuck_policy & KEY_STUCK_POLICY_ASK_VETO) != 0 &&
            pk_app_should_deep_sleep(&s_app)) {
            (void)app_ports_enter_deep_sleep();
        }
        if ((stuck_policy & KEY_STUCK_POLICY_SUPPRESS_NEW) != 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if ((uint32_t)(now - last_batt_ms) >= 1000u) {
            last_batt_ms = now;
            app_ports_refresh_battery(&s_app);
            {
                bool desktop = usb_serial_jtag_is_connected();
                if (desktop != was_desktop) {
                    was_desktop = desktop;
                    pk_app_set_desktop(&s_app, desktop);
                    if (s_app.awake) {
                        bsp_display_backlight(effective_backlight_percent(&s_app));
                    }
                    ESP_LOGI(TAG, "desktop power link %s", desktop ? "on" : "off");
                }
            }
            if (s_app.low_battery != was_low_batt) {
                was_low_batt = s_app.low_battery;
                if (s_app.awake) {
                    bsp_display_backlight(effective_backlight_percent(&s_app));
                }
            }
        }

        if (s_app.awake) {
            const bool waking = !was_awake;
            if ((waking || s_app.lock_alert_active || app_ui_needs_update(&s_app)) &&
                bsp_lvgl_lock(20)) {
                app_ui_update(&s_app);
                if (waking || s_app.lock_alert_active) {
                    lv_refr_now(NULL);
                }
                bsp_lvgl_unlock();
            }
            if (waking || s_app.lock_alert_active) {
                bsp_display_backlight(effective_backlight_percent(&s_app));
            }
        }
        was_awake = s_app.awake;
        {
            const bool input_active = key_pressed || s_app.has_pending ||
                                      s_app.keys.down || s_app.keys.deadline_valid ||
                                      s_app.side.active ||
                                      s_btn.pending;
            const uint32_t wait_ms = input_active ? 10u : (s_app.awake ? 40u : 80u);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        }
    }
}
