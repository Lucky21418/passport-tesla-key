#include "port_internal.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_BSP_ENABLE_AUDIO
#include "bsp_audio.h"
#endif
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ports";

#define PORT_RX_POOL_SLOTS     4
#define BTN_SAMPLE_PERIOD_US   5000

typedef struct {
    bool    in_use;
    uint8_t data[PK_PROTO_MAX_FRAME_LEN];
} port_rx_slot_t;

static QueueHandle_t        s_ev_queue;
static TaskHandle_t         s_main_task;
static pk_app_t            *s_bound_app;
static pk_ble_adapter_t    *s_bound_ble;
static port_rx_slot_t       s_rx_pool[PORT_RX_POOL_SLOTS];
static portMUX_TYPE         s_rx_pool_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t   s_btn_timer;
static pk_btn_sample_ring_t s_btn_ring;
static portMUX_TYPE         s_btn_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int         s_latest_btn_mv = -1;

pk_ms_t port_now_ms(void)
{
    return (pk_ms_t)(esp_timer_get_time() / 1000);
}

static void btn_sample_timer_cb(void *arg)
{
    int s0, s1, s2, mv;
    bool valid = false;
    bool edge_changed = false;
    pk_ms_t ts;

    (void)arg;
    ts = port_now_ms();
    s0 = bsp_button_read_mv();
    s1 = bsp_button_read_mv();
    s2 = bsp_button_read_mv();
    mv = pk_btn_median3(s0, s1, s2, &valid);
    s_latest_btn_mv = valid ? mv : -1;

    portENTER_CRITICAL(&s_btn_mux);
    edge_changed = pk_btn_ring_push(&s_btn_ring, ts, mv, valid);
    portEXIT_CRITICAL(&s_btn_mux);

    if (edge_changed && s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

static void stop_btn_sample_timer(void)
{
    if (!s_btn_timer) {
        return;
    }
    (void)esp_timer_stop(s_btn_timer);
    (void)esp_timer_delete(s_btn_timer);
    s_btn_timer = NULL;
}

bool app_ports_pop_button_sample(pk_btn_sample_t *out)
{
    bool ok;

    portENTER_CRITICAL(&s_btn_mux);
    ok = pk_btn_ring_pop(&s_btn_ring, out);
    portEXIT_CRITICAL(&s_btn_mux);
    return ok;
}

int app_ports_latest_button_mv(void)
{
    int mv = s_latest_btn_mv;
    if (mv >= 0) {
        return mv;
    }
    return bsp_button_read_mv();
}

int port_rx_buf_alloc_copy(const uint8_t *data, size_t len)
{
    int idx = -1;
    int i;

    if (!data || len == 0 || len > PK_PROTO_MAX_FRAME_LEN) {
        return -1;
    }
    portENTER_CRITICAL(&s_rx_pool_mux);
    for (i = 0; i < PORT_RX_POOL_SLOTS; i++) {
        if (!s_rx_pool[i].in_use) {
            s_rx_pool[i].in_use = true;
            idx = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_rx_pool_mux);
    if (idx < 0) {
        return -1;
    }
    memcpy(s_rx_pool[idx].data, data, len);
    return idx;
}

const uint8_t *port_rx_buf_ptr(int idx)
{
    if (idx < 0 || idx >= PORT_RX_POOL_SLOTS) {
        return NULL;
    }
    return s_rx_pool[idx].data;
}

void port_rx_buf_release(int idx)
{
    if (idx < 0 || idx >= PORT_RX_POOL_SLOTS) {
        return;
    }
    portENTER_CRITICAL(&s_rx_pool_mux);
    s_rx_pool[idx].in_use = false;
    portEXIT_CRITICAL(&s_rx_pool_mux);
}

static uint32_t s_ev_queue_drops = 0;

bool port_post_event(const port_event_t *ev)
{
    if (!s_ev_queue || !ev) {
        return false;
    }
    if (xQueueSend(s_ev_queue, ev, 0) != pdTRUE) {
        s_ev_queue_drops++;
        ESP_LOGW(TAG, "event queue full: type=%d drops=%u",
                 (int)ev->type, (unsigned)s_ev_queue_drops);
        return false;
    }
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
    return true;
}

pk_app_t *port_bound_app(void)
{
    return s_bound_app;
}

pk_ble_adapter_t *port_bound_ble(void)
{
    return s_bound_ble;
}

void app_ports_format_masked_vin(const char *vin, char *out, size_t out_len)
{
    size_t len;
    if (!out || out_len == 0) {
        return;
    }
    if (!vin || vin[0] == '\0') {
        snprintf(out, out_len, "未录入");
        return;
    }
    len = strlen(vin);
    if (len >= 4) {
        snprintf(out, out_len, "***%s", vin + len - 4);
    } else {
        snprintf(out, out_len, "***");
    }
}


/* ==========================================================================
 * Button ADC, Backlight & Fuel Gauge Ports
 * ========================================================================== */

static int adc_read_mv(void *ctx, int *mv, bool *valid)
{
    int sample;

    (void)ctx;
    if (!mv || !valid) {
        return -1;
    }
    sample = bsp_button_read_mv();
    if (sample < 0) {
        *mv = 0;
        *valid = false;
        return -1;
    }
    *mv = sample;
    *valid = true;
    return 0;
}

static pk_err_t backlight_set_percent(void *ctx, int percent)
{
    const pk_app_t *app = port_bound_app();

    (void)ctx;
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* Low battery owns a backlight floor on every path, not only wake/alert. */
    if (app && app->low_battery && !app->lock_alert_active &&
        !app->moving && !app->desktop &&
        percent > PK_BRIGHTNESS_MIN) {
        percent = PK_BRIGHTNESS_MIN;
    }
    bsp_display_backlight((uint8_t)percent);
    return PK_OK;
}

static pk_err_t backlight_set_awake(void *ctx, bool awake)
{
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    (void)ctx;
    if (awake && panel) {
        (void)esp_lcd_panel_disp_on_off(panel, true);
    }
    if (!awake) {
        bsp_display_backlight(0);
        if (panel) {
            (void)esp_lcd_panel_disp_on_off(panel, false);
        }
    }
    return PK_OK;
}

static const pk_backlight_port_t k_backlight = {
    .set_percent = backlight_set_percent,
    .set_awake = backlight_set_awake,
};

static pk_err_t fuel_read(void *ctx, bool *present, int *percent)
{
    int soc;

    (void)ctx;
    if (!present || !percent) {
        return PK_ERR_INVALID;
    }
    soc = bsp_battery_soc();
    if (soc < 0) {
        *present = false;
        *percent = 0;
        return PK_ERR_NOT_WIRED;
    }
    *present = true;
    *percent = soc;
    return PK_OK;
}

static const pk_fuel_port_t k_fuel = {
    .read = fuel_read,
};

void app_ports_init(pk_app_t *app, pk_btn_adapter_t *btn, pk_ble_adapter_t *ble)
{
    pk_hal_t hal;
    pk_store_image_t recovered = {0};

    s_bound_app = app;
    s_bound_ble = ble;
    s_main_task = xTaskGetCurrentTaskHandle();
    if (!s_ev_queue) {
        s_ev_queue = xQueueCreate(16, sizeof(port_event_t));
    }

    pk_hal_init_unwired(&hal);
    pk_app_caps_clear(&app->caps);

    pk_btn_adapter_init(btn, app);
    btn->adc.read_mv = adc_read_mv;
    btn->adc.read_ctx = NULL;

    portENTER_CRITICAL(&s_btn_mux);
    pk_btn_ring_init(&s_btn_ring);
    portEXIT_CRITICAL(&s_btn_mux);
    s_latest_btn_mv = bsp_button_read_mv();
    if (!s_btn_timer) {
        const esp_timer_create_args_t tcfg = {
            .callback = btn_sample_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pk_btn_5ms",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&tcfg, &s_btn_timer) == ESP_OK) {
            if (esp_timer_start_periodic(s_btn_timer, BTN_SAMPLE_PERIOD_US) != ESP_OK) {
                (void)esp_timer_delete(s_btn_timer);
                s_btn_timer = NULL;
            }
        } else {
            s_btn_timer = NULL;
        }
    }

    port_ble_init_and_attach(app, ble);

    hal.ble = app->hal.ble;
    hal.ble_ctx = app->hal.ble_ctx;
    hal.nvs = port_nvs_available() ? port_nvs_ops() : NULL;
    hal.softap = port_softap_ops();
    hal.backlight = &k_backlight;
    hal.fuel = &k_fuel;
    pk_hal_attach(app, &hal);

    /* Hydrate NVS store & per-slot keys */
    port_nvs_load_slot_keys();
    pk_store_recover(&app->store, &recovered);
    if (app->store.committed) {
        app->brightness = (recovered.brightness >= PK_BRIGHTNESS_MIN &&
                           recovered.brightness <= PK_BRIGHTNESS_MAX)
                              ? recovered.brightness
                              : PK_BRIGHTNESS_DEFAULT;
        app->power = recovered.power;
        if (recovered.current >= 0 && recovered.current < PK_MAX_VEHICLES &&
            recovered.vehicles[recovered.current].used) {
            app->vehicle = recovered.current;
            app->paired = pk_store_slot_key_ready(&app->store, recovered.current);
            app->car_focus = recovered.current;
            if (!app->paired) {
                ESP_LOGW(TAG, "saved slot %d keypair unavailable or corrupted on boot",
                         recovered.current);
                pk_app_set_msg(app, PK_MSG_NOT_PAIRED);
            }
        }
        bsp_display_backlight((uint8_t)app->brightness);
    }

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 unavailable; battery remains unknown (never 0%%)");
    }
    app_ports_refresh_battery(app);
}

void app_ports_poll_buttons(pk_btn_adapter_t *btn, pk_ms_t now)
{
    int prev_locked = btn ? btn->locked : -1;

    (void)pk_btn_adapter_poll(btn, now);
    if (btn && btn->locked != prev_locked) {
        ESP_LOGI(TAG, "btn locked=%d page=%d", btn->locked, s_bound_app ? (int)s_bound_app->page : -1);
    }
}

void app_ports_poll_events(pk_app_t *app, pk_ms_t now)
{
    port_event_t ev;

    if (!app || !s_ev_queue) {
        return;
    }

    while (xQueueReceive(s_ev_queue, &ev, 0) == pdTRUE) {
        if (ev.type == PORT_EV_HTTP_VIN_SUBMIT) {
            if (app->page == PK_PAGE_VIN) {
                port_ble_start_candidate_pairing(app, ev.u.vin);
            }
            memset(ev.u.vin, 0, sizeof(ev.u.vin));
        } else {
            port_ble_handle_event(app, &ev, now);
        }
    }

    port_ble_poll(app, now);
}

#define BATT_LOW_ENTER_PCT      5
#define BATT_LOW_EXIT_PCT       10
#define BATT_LOW_DEBOUNCE_COUNT 3
#define BATT_ERR_DEBOUNCE_COUNT 3
#define BATT_GLITCH_MV_MIN      3650

typedef struct {
    bool    has_valid;
    int     last_percent;
    uint8_t err_streak;
    uint8_t low_streak;
} batt_filter_t;

static batt_filter_t s_batt_filter;

void app_ports_refresh_battery(pk_app_t *app)
{
    bool present = false;
    int percent = 0;

    if (!app) {
        return;
    }

    if (fuel_read(NULL, &present, &percent) != PK_OK || !present) {
        if (s_batt_filter.err_streak < BATT_ERR_DEBOUNCE_COUNT) {
            s_batt_filter.err_streak++;
        }
        if (!s_batt_filter.has_valid || s_batt_filter.err_streak >= BATT_ERR_DEBOUNCE_COUNT) {
            s_batt_filter.has_valid = false;
            s_batt_filter.low_streak = 0;
            pk_app_battery(app, false, 0, false);
        }
        return;
    }

    s_batt_filter.err_streak = 0;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    {
        int mv = bsp_battery_mv();
        if (percent <= BATT_LOW_ENTER_PCT && mv >= BATT_GLITCH_MV_MIN) {
            ESP_LOGW(TAG, "忽略异常低电量采样: soc=%d%% vcell=%d mV", percent, mv);
            s_batt_filter.low_streak = 0;
            if (percent == 0 && s_batt_filter.has_valid) {
                return;
            }
            pk_app_battery(app, true, percent, app->low_battery);
            return;
        }
    }

    s_batt_filter.has_valid = true;
    s_batt_filter.last_percent = percent;

    {
        bool next_low = app->low_battery;
        if (percent <= BATT_LOW_ENTER_PCT) {
            if (s_batt_filter.low_streak < BATT_LOW_DEBOUNCE_COUNT) {
                s_batt_filter.low_streak++;
            }
            if (s_batt_filter.low_streak >= BATT_LOW_DEBOUNCE_COUNT) {
                next_low = true;
            }
        } else {
            s_batt_filter.low_streak = 0;
            if (app->low_battery && percent >= BATT_LOW_EXIT_PCT) {
                next_low = false;
            }
        }
        pk_app_battery(app, true, percent, next_low);
    }
}

bool app_ports_ble_rssi(int8_t *out_rssi)
{
    return port_ble_rssi(out_rssi);
}

int app_ports_ble_pair_state(const pk_app_t *app)
{
    return port_ble_pair_state(app);
}

void app_ports_stop_ble_scan(void)
{
    port_ble_stop_scan();
}

void app_ports_set_screen_awake(bool awake)
{
    (void)backlight_set_awake(NULL, awake);
}

bool app_ports_enter_deep_sleep(void)
{
    int mv = bsp_button_read_mv();
    int level = 0;
    esp_err_t err;

    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        return false;
    }

    ESP_LOGI(TAG, "进入续航深度休眠 (GPIO0 低电平唤醒)");
    /* Stop and delete the 5 ms button ADC sampler BEFORE tearing down the
     * shared ADC unit in bsp_button_prepare_deep_sleep(). */
    stop_btn_sample_timer();
    port_ble_deinit();

    /* The button must be released before the teardown starts. A held key would
     * wake the chip about a second after sleep, so refuse to enter instead of
     * tearing down every peripheral and hoping. A restart is the only clean
     * recovery here because the button driver and BLE are already stopped. */
    err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK || level != 1) {
        ESP_LOGE(TAG, "按键未释放或交接失败(level=%d): 取消深睡并重启", level);
        esp_restart();
        return false;
    }

    err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO 唤醒配置失败: %s", esp_err_to_name(err));
    }

    /* Five-step power closure. The order matters: CW2017 and ES8311 both live
     * on I2C0, so both must be suspended before the bus pins are parked; the
     * I2S pins are released before the display/SPI pins. These calls are no-ops
     * when the audio subsystem was never initialised. */
    if (bsp_battery_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 休眠校验未通过，继续入睡");
    }
#if CONFIG_BSP_ENABLE_AUDIO
    /* Stable builds do not compile the audio driver (no codec/I2S is ever
     * opened). Pro/audio builds must suspend ES8311 and park the I2S pins here,
     * before the shared I2C bus is released. */
    if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 挂起校验未通过，继续入睡");
    }
    (void)bsp_audio_prepare_deep_sleep();
#endif
    (void)bsp_i2c_prepare_deep_sleep();

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏，重启恢复");
        esp_restart();
        return false;
    }
    (void)bsp_display_prepare_deep_sleep();

    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回，重启恢复");
    esp_restart();
    return true;
}
