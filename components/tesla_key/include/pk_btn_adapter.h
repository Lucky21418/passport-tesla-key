#ifndef PK_BTN_ADAPTER_H
#define PK_BTN_ADAPTER_H

#include "pk_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M1 button adapter: turn FoloToy raw edges into pk_app_button_edge calls.
 *
 * Preferred path: millivolt samples (GPIO0 / ADC1_CH0 resistor ladder) go
 * through the voltage-lock decoder (CONFIRM 40 ms / HOLD 120 ms) and only
 * then become PRESS / RELEASE. Do not instantiate iot_button with
 * BSP_BTN_SHORT_PRESS_MS (180) / BSP_BTN_LONG_PRESS_MS (500), and do not
 * translate BUTTON_SINGLE_CLICK / DOUBLE_CLICK / LONG_PRESS_START into
 * application edges.
 *
 * Physical order on the product, top to bottom, is ▲ / ● / ▼ (confirmed on
 * hardware 2026-11-17; idle node 2898 mV):
 *   0 = ▲  (0 Ω, 0–150 mV)
 *   1 = ●  (1 kΩ, 150–447 mV)
 *   2 = ▼  (2.2 kΩ, 447–1900 mV)
 *   ≥ 1900 mV is released. A failed or negative reading is never a key.
 *
 * The left power key is a separate GPIO, not on this ladder.
 *
 * ADC / GPIO are not physically attached in this phase. Feeding samples
 * is how a later poller or a host harness talks to the app. A NULL read
 * hook returns PK_ERR_NOT_WIRED and invents no press.
 *
 * 待真机: window mid-points on the current board, 40/120 ms compensation
 * versus the 180 ms / 500 ms contract, missed edges under BLE / draw
 * load, GPIO0 deep-sleep wake vs first-cycle wake-only.
 */

#define PK_BTN_LADDER_COUNT     3
#define PK_BTN_MV_RELEASED_MIN  1900

typedef enum {
    PK_BTN_SRC_PRESS = 0,      /* BSP_BTN_PRESS / BUTTON_PRESS_DOWN */
    PK_BTN_SRC_RELEASE,        /* BSP_BTN_RELEASE / BUTTON_PRESS_UP */
    /* The following are official cooked events and MUST be dropped. */
    PK_BTN_SRC_COOKED_CLICK,   /* BSP_BTN_CLICK / BUTTON_SINGLE_CLICK (180 ms) */
    PK_BTN_SRC_COOKED_DOUBLE,  /* BSP_BTN_DOUBLE / BUTTON_DOUBLE_CLICK */
    PK_BTN_SRC_COOKED_LONG     /* BSP_BTN_LONG / BUTTON_LONG_PRESS_START (500 ms) */
} pk_btn_src_ev_t;

typedef struct {
    /* Optional later ADC hook. Return 0 and set *mv on success; non-zero
     * or *valid=false is a failed reading, never an UP press at 0 mV.
     */
    int (*read_mv)(void *ctx, int *mv, bool *valid);
    void *read_ctx;
} pk_btn_adc_port_t;

typedef struct {
    pk_app_t *app;
    pk_btn_adc_port_t adc;

    int locked;           /* ladder index or -1 */
    int candidate;
    bool pending;
    bool candidate_clean_release; /* true only when candidate==-1 AND every sample in the streak had valid mv >= 1900 */
    pk_ms_t since_ms;
    bool time_set;
    pk_ms_t last_ms;

    bool down[PK_BTN_LADDER_COUNT]; /* last emitted press state */
    bool power_down;

    unsigned dropped_cooked;
    unsigned dropped_invalid;
} pk_btn_adapter_t;

void pk_btn_adapter_init(pk_btn_adapter_t *ad, pk_app_t *app);

/* Voltage-lock decoder. mv < 0 or valid=false is never a key press. */
pk_err_t pk_btn_adapter_feed_mv(pk_btn_adapter_t *ad, pk_ms_t now, int mv, bool valid);

/* If an ADC port is attached, read once and feed the decoder. */
pk_err_t pk_btn_adapter_poll(pk_btn_adapter_t *ad, pk_ms_t now);

/*
 * Safety net if a future glue still sees official BSP callbacks.
 * Only PRESS / RELEASE are forwarded. Click / double / long are counted
 * and dropped. Prefer not to register those callbacks at all.
 */
pk_err_t pk_btn_adapter_on_bsp_event(pk_btn_adapter_t *ad, pk_ms_t now,
                                     int ladder_index, pk_btn_src_ev_t ev);

/* Independent left power key. */
pk_err_t pk_btn_adapter_power_edge(pk_btn_adapter_t *ad, pk_ms_t now, pk_edge_t edge);

pk_key_id_t pk_btn_ladder_key(int ladder_index);

/* Pure 3-sample median filter for GPIO0 ADC readings; preserves invalid (-1)
 * when fewer than 2 of 3 samples are valid so a failed ADC burst never turns
 * into a 0 mV UP press or a synthetic valid release.
 */
int pk_btn_median3(int s0, int s1, int s2, bool *out_valid);

#define PK_BTN_RING_CAP 64u

typedef struct {
    pk_ms_t ts_ms;
    int16_t mv;
    bool    valid;
} pk_btn_sample_t;

typedef struct {
    pk_btn_sample_t buf[PK_BTN_RING_CAP];
    uint8_t         head;
    uint8_t         count;
    int8_t          last_window;
    bool            has_last_window;
} pk_btn_sample_ring_t;

void pk_btn_ring_init(pk_btn_sample_ring_t *ring);
/* Push one timestamped sample; returns true when the decoded voltage window
 * changed so an ISR / esp_timer producer knows when to wake the main task.
 * Oldest sample is overwritten on overflow so recent edges are never lost.
 */
bool pk_btn_ring_push(pk_btn_sample_ring_t *ring, pk_ms_t ts_ms, int mv, bool valid);
bool pk_btn_ring_pop(pk_btn_sample_ring_t *ring, pk_btn_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PK_BTN_ADAPTER_H */
