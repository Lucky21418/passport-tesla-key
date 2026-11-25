#include "pk_btn_adapter.h"

#include <string.h>

/*
 * Voltage windows from the official FoloToy bsp_pins.h snapshot
 * (BSP_BTN_MV_TABLE, half-open). Mid-points remain 待真机 on the
 * current board; this adapter does not retune them.
 */
static const int k_mv_min[PK_BTN_LADDER_COUNT] = { 0, 150, 447 };
static const int k_mv_max[PK_BTN_LADDER_COUNT] = { 150, 447, 1900 };

pk_key_id_t pk_btn_ladder_key(int ladder_index)
{
    /* Measured on FoloToy AI Passport hardware (2026-11-16), re-confirmed with a
     * read-only ADC probe on 2026-11-17 (idle 2898 mV):
     *   0 = ▲ (top):    ~4 mV   (0..150 mV)
     *   1 = ● (middle): ~303 mV (150..447 mV)
     *   2 = ▼ (bottom): ~599 mV (447..1900 mV)
     */
    switch (ladder_index) {
    case 0:
        return PK_KEY_UP;
    case 1:
        return PK_KEY_DOT;
    case 2:
        return PK_KEY_DOWN;
    default:
        return PK_KEY_NONE;
    }
}

void pk_btn_adapter_init(pk_btn_adapter_t *ad, pk_app_t *app)
{
    if (!ad) {
        return;
    }
    memset(ad, 0, sizeof(*ad));
    ad->app = app;
    ad->locked = -1;
    ad->candidate = -1;
}

static int window_of(int mv, bool valid)
{
    int i;
    /* Read / conversion failure is never a key, including never a 0 mV UP. */
    if (!valid || mv < 0) {
        return -1;
    }
    if (mv >= PK_BTN_MV_RELEASED_MIN) {
        return -1;
    }
    for (i = 0; i < PK_BTN_LADDER_COUNT; i++) {
        if (mv >= k_mv_min[i] && mv < k_mv_max[i]) {
            return i;
        }
    }
    return -1;
}

static pk_err_t emit(pk_btn_adapter_t *ad, pk_ms_t now, int index, pk_edge_t edge)
{
    pk_key_id_t key = pk_btn_ladder_key(index);
    if (!ad->app || key == PK_KEY_NONE) {
        return PK_ERR_INVALID;
    }
    if (edge == PK_EDGE_PRESS) {
        if (ad->down[index]) {
            return PK_OK;
        }
        ad->down[index] = true;
    } else {
        if (!ad->down[index]) {
            return PK_OK;
        }
        ad->down[index] = false;
    }
    return pk_app_button_edge(ad->app, now, key, edge);
}

static pk_err_t emit_lock_change(pk_btn_adapter_t *ad, pk_ms_t now, int previous, int locked)
{
    pk_err_t e = PK_OK;
    if (previous == locked) {
        return PK_OK;
    }
    if (previous >= 0) {
        e = emit(ad, now, previous, PK_EDGE_RELEASE);
        if (e != PK_OK) {
            return e;
        }
    }
    if (locked >= 0) {
        e = emit(ad, now, locked, PK_EDGE_PRESS);
    }
    return e;
}

pk_err_t pk_btn_adapter_feed_mv(pk_btn_adapter_t *ad, pk_ms_t now, int mv, bool valid)
{
    int window;
    int previous;
    bool clean_release_sample = false;
    pk_ms_t need;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    if (ad->time_set && pk_time_before(now, ad->last_ms)) {
        return PK_ERR_INVALID;
    }
    ad->last_ms = now;
    ad->time_set = true;

    if (!valid || mv < 0) {
        ad->dropped_invalid++;
        window = -1;
    } else {
        window = window_of(mv, true);
        clean_release_sample = (window < 0 && mv >= PK_BTN_MV_RELEASED_MIN);
    }

    /* Voltage lock: idle new window must stay stable CONFIRM_MS (40 ms) before
     * it is a press; once locked, cross-window sweeps, ▲/▼ holds, and invalid/
     * failed ADC samples must stay stable HOLD_MS (120 ms) before accepted.
     * Only when ● returns cleanly to the valid >= 1900 mV idle band for every
     * sample in the streak does release confirm at PK_BTN_DOT_RELEASE_MS (40 ms),
     * preserving fast double-clicks and symmetric hold duration.
     */
    if (window == ad->locked) {
        ad->pending = false;
        ad->candidate = -1;
        ad->candidate_clean_release = false;
        return PK_OK;
    }
    if (!ad->pending || window != ad->candidate) {
        ad->pending = true;
        ad->candidate = window;
        ad->candidate_clean_release = clean_release_sample;
        ad->since_ms = now;
        return PK_OK;
    }
    if (!clean_release_sample) {
        ad->candidate_clean_release = false;
    }
    if (ad->locked < 0) {
        need = (pk_ms_t)PK_BSP_BTN_CONFIRM_MS;
    } else if (pk_btn_ladder_key(ad->locked) == PK_KEY_DOT &&
               ad->candidate < 0 && ad->candidate_clean_release) {
        need = (pk_ms_t)PK_BTN_DOT_RELEASE_MS;
    } else {
        need = (pk_ms_t)PK_BSP_BTN_HOLD_MS;
    }
    if (!pk_time_elapsed(now, ad->since_ms, need)) {
        return PK_OK;
    }
    previous = ad->locked;
    ad->locked = ad->candidate;
    ad->pending = false;
    ad->candidate = -1;
    ad->candidate_clean_release = false;
    return emit_lock_change(ad, now, previous, ad->locked);
}

pk_err_t pk_btn_adapter_poll(pk_btn_adapter_t *ad, pk_ms_t now)
{
    int mv = 0;
    bool valid = false;
    int rc;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    if (!ad->adc.read_mv) {
        return PK_ERR_NOT_WIRED;
    }
    rc = ad->adc.read_mv(ad->adc.read_ctx, &mv, &valid);
    if (rc != 0) {
        return pk_btn_adapter_feed_mv(ad, now, -1, false);
    }
    return pk_btn_adapter_feed_mv(ad, now, mv, valid);
}

pk_err_t pk_btn_adapter_on_bsp_event(pk_btn_adapter_t *ad, pk_ms_t now,
                                     int ladder_index, pk_btn_src_ev_t ev)
{
    pk_key_id_t key;

    if (!ad) {
        return PK_ERR_INVALID;
    }
    if (ev == PK_BTN_SRC_COOKED_CLICK || ev == PK_BTN_SRC_COOKED_DOUBLE ||
        ev == PK_BTN_SRC_COOKED_LONG) {
        /* The app owns the raw-edge 300 ms / 500 ms contract. Cooked
         * iot_button events would pre-empt it; drop them rather than
         * wrapping another click/long state machine.
         */
        ad->dropped_cooked++;
        return PK_ERR_REJECTED;
    }
    if (ev != PK_BTN_SRC_PRESS && ev != PK_BTN_SRC_RELEASE) {
        ad->dropped_invalid++;
        return PK_ERR_INVALID;
    }
    key = pk_btn_ladder_key(ladder_index);
    if (key == PK_KEY_NONE || !ad->app) {
        return PK_ERR_INVALID;
    }
    /* If a glue still uses BUTTON_PRESS_DOWN / UP from iot_button, those
     * edges already include the 40 / 120 ms lock delay. Do not run them
     * through the decoder a second time. Prefer feed_mv / poll instead,
     * and do not register click / double / long callbacks.
     */
    return emit(ad, now, ladder_index, ev == PK_BTN_SRC_PRESS ? PK_EDGE_PRESS : PK_EDGE_RELEASE);
}

pk_err_t pk_btn_adapter_power_edge(pk_btn_adapter_t *ad, pk_ms_t now, pk_edge_t edge)
{
    if (!ad || !ad->app) {
        return PK_ERR_INVALID;
    }
    if (edge != PK_EDGE_PRESS && edge != PK_EDGE_RELEASE) {
        return PK_ERR_INVALID;
    }
    if (edge == PK_EDGE_PRESS) {
        if (ad->power_down) {
            return PK_OK;
        }
        ad->power_down = true;
    } else {
        if (!ad->power_down) {
            return PK_OK;
        }
        ad->power_down = false;
    }
    return pk_app_button_edge(ad->app, now, PK_KEY_POWER, edge);
}

int pk_btn_median3(int s0, int s1, int s2, bool *out_valid)
{
    int vals[3];
    int n = 0;

    if (s0 >= 0) {
        vals[n++] = s0;
    }
    if (s1 >= 0) {
        vals[n++] = s1;
    }
    if (s2 >= 0) {
        vals[n++] = s2;
    }
    if (n < 2) {
        if (out_valid) {
            *out_valid = false;
        }
        return -1;
    }
    if (out_valid) {
        *out_valid = true;
    }
    if (n == 2) {
        return (vals[0] + vals[1]) / 2;
    }
    if (vals[0] > vals[1]) {
        int tmp = vals[0];
        vals[0] = vals[1];
        vals[1] = tmp;
    }
    if (vals[1] > vals[2]) {
        int tmp = vals[1];
        vals[1] = vals[2];
        vals[2] = tmp;
    }
    if (vals[0] > vals[1]) {
        int tmp = vals[0];
        vals[0] = vals[1];
        vals[1] = tmp;
    }
    return vals[1];
}

void pk_btn_ring_init(pk_btn_sample_ring_t *ring)
{
    if (!ring) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    ring->last_window = -1;
}

bool pk_btn_ring_push(pk_btn_sample_ring_t *ring, pk_ms_t ts_ms, int mv, bool valid)
{
    uint8_t write_idx;
    int8_t win;
    bool changed;

    if (!ring) {
        return false;
    }
    if (mv > 32767) {
        mv = 32767;
    } else if (mv < -1) {
        mv = -1;
    }
    win = (int8_t)window_of(mv, valid);
    changed = !ring->has_last_window || (win != ring->last_window);
    ring->last_window = win;
    ring->has_last_window = true;

    if (ring->count >= PK_BTN_RING_CAP) {
        /* Overwrite oldest so fresh edge timestamps are never lost under a stall. */
        write_idx = ring->head;
        ring->head = (uint8_t)((ring->head + 1u) % PK_BTN_RING_CAP);
    } else {
        write_idx = (uint8_t)((ring->head + ring->count) % PK_BTN_RING_CAP);
        ring->count++;
    }
    ring->buf[write_idx] = (pk_btn_sample_t){
        .ts_ms = ts_ms,
        .mv = (int16_t)mv,
        .valid = valid && mv >= 0,
    };
    return changed;
}

bool pk_btn_ring_pop(pk_btn_sample_ring_t *ring, pk_btn_sample_t *out)
{
    if (!ring || ring->count == 0u) {
        return false;
    }
    if (out) {
        *out = ring->buf[ring->head];
    }
    ring->head = (uint8_t)((ring->head + 1u) % PK_BTN_RING_CAP);
    ring->count--;
    return true;
}
