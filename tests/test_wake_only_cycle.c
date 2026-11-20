/*
 * Host test for the screen-off "first press only wakes" contract (HANDOFF §3
 * journey 1). The device may sit for hours with the panel off; the first key
 * cycle after a blank screen (or after a power-key wake) must not send a
 * vehicle command, start a qualification read, or show a "pressed" hint.
 *
 * This is the only host coverage for that invariant, so it drives the real key
 * state machine (pk_keys_dot_press / pk_keys_dot_release / pk_side_press) with
 * a recording radio and asserts that no frame is written.
 */

#include "pk_app.h"
#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_copy.h"

#include <stdio.h>

static int g_fail;
static unsigned g_writes;

static void expect(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static pk_err_t record_write(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    g_writes++;
    return PK_OK;
}

static pk_err_t fake_connect(void *ctx, int slot, pk_ms_t budget)
{
    (void)ctx;
    (void)slot;
    (void)budget;
    return PK_OK;
}

static pk_err_t fake_disconnect(void *ctx, int slot)
{
    (void)ctx;
    (void)slot;
    return PK_OK;
}

static pk_err_t fake_cancel(void *ctx, pk_id_t txn)
{
    (void)ctx;
    (void)txn;
    return PK_OK;
}

static void setup(pk_app_t *app, pk_ble_adapter_t *ad)
{
    pk_app_init(app, PK_VEHICLE_NONE, false);
    pk_ble_adapter_init(ad);
    ad->radio.write_once = record_write;
    ad->radio.connect_once = fake_connect;
    ad->radio.disconnect = fake_disconnect;
    ad->radio.cancel = fake_cancel;
    pk_ble_adapter_attach(app, ad);
    app->now = 1000;
    app->awake = false;
    g_writes = 0;
}

static void test_dot_first_cycle_only_wakes(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    pk_keys_dot_press(&app, 1000);
    expect(app.awake, "wake-only: first press lights the screen");
    expect(app.keys.wake_cycle, "wake-only: first cycle is a wake cycle");
    expect(app.keys.consumed, "wake-only: first cycle is consumed");
    expect(app.message == PK_MSG_EMPTY, "wake-only: no pressed hint while waking");
    expect(g_writes == 0, "wake-only: no BLE frame on the wake press");

    pk_keys_dot_release(&app, 1100);
    expect(!app.keys.wake_cycle, "wake-only: release ends the wake cycle");
    expect(!app.keys.down, "wake-only: key is released");
    expect(!app.consume_as_wake_only, "wake-only: later gestures are allowed");
    expect(g_writes == 0, "wake-only: no BLE frame on the wake release");
    expect(app.message == PK_MSG_EMPTY, "wake-only: still no pressed hint");

    /* The next complete cycle is a normal gesture again. */
    pk_keys_dot_press(&app, 2000);
    expect(!app.keys.wake_cycle, "wake-only: second press is not a wake cycle");
    expect(!app.keys.consumed, "wake-only: second press arms a gesture");
    expect(app.message == PK_MSG_PRESSED, "wake-only: second press shows the hint");
    pk_keys_dot_release(&app, 2100);
    expect(!app.keys.down && app.keys.deadline_valid,
           "wake-only: release arms the double-click window");
}

static void test_side_first_cycle_only_wakes(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    expect(!pk_side_press(&app, PK_KEY_UP, 1000),
           "wake-only: side press on a dark screen is not a command");
    expect(app.awake, "wake-only: side press lights the screen");
    expect(app.side.consumed, "wake-only: side wake cycle is consumed");
    expect(g_writes == 0, "wake-only: no BLE frame from the side wake");
    pk_side_release(&app, PK_KEY_UP, 1100);
    expect(g_writes == 0, "wake-only: side release sends nothing");
    expect(app.message == PK_MSG_EMPTY, "wake-only: no cancelled hint for a wake cycle");
}

static void test_power_key_wake_still_costs_one_cycle(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    pk_power_press(&app, 1000);
    expect(app.awake && app.consume_as_wake_only,
           "wake-only: power key arms a one-cycle guard");
    pk_keys_dot_press(&app, 1100);
    expect(app.keys.wake_cycle, "wake-only: function key after power wake only wakes");
    expect(g_writes == 0, "wake-only: power-then-dot sends no frame");
    pk_keys_dot_release(&app, 1200);
    expect(!app.consume_as_wake_only,
           "wake-only: the guard is cleared after one complete cycle");
}

int main(void)
{
    test_dot_first_cycle_only_wakes();
    test_side_first_cycle_only_wakes();
    test_power_key_wake_still_costs_one_cycle();

    if (g_fail) {
        fprintf(stderr, "test_wake_only_cycle: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_wake_only_cycle: ok\n");
    return 0;
}
