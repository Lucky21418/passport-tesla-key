/*
 * Host tests for the 2026-11-18 gesture/confirmation contract.
 *
 * Grammar: short = act, double = back/status, long = high-consequence action.
 * Timing: 180 ms double window / 500 ms hold (FoloToy BSP + shipped 1234/1149).
 * ACK is terminal for the app; state is refreshed by explicit reads, never by
 * a timer-based readback.
 */

#include "pk_app.h"
#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_protocol.h"
#include "car_server.pb.h"
#include "pb_decode.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
static unsigned g_writes;
static pk_action_t g_last_action;

static void expect(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static pk_err_t capture_write(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    g_writes++;
    if (frame) {
        g_last_action = frame->action;
    }
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

static bool fake_has_session(void *ctx, int slot, pk_domain_t domain)
{
    (void)ctx;
    (void)slot;
    (void)domain;
    return true;
}

static void setup(pk_app_t *app, pk_ble_adapter_t *ad)
{
    pk_app_init(app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(ad);
    ad->radio.write_once = capture_write;
    ad->radio.connect_once = fake_connect;
    ad->radio.disconnect = fake_disconnect;
    ad->radio.cancel = fake_cancel;
    ad->radio.has_session = fake_has_session;
    pk_ble_adapter_attach(app, ad);
    expect(pk_store_add_verified(&app->store, "5YJ3E1EA1PF000001", PK_STORE_CRASH_NONE),
           "store add");
    {
        int slot = pk_store_find_vin(&app->store, "5YJ3E1EA1PF000001");
        expect(slot >= 0 && pk_app_select_vehicle(app, slot), "select vehicle");
    }
    ad->connected_slot = app->vehicle;
    pk_app_set_connected(app, true);
    pk_app_tick(app, 1000);
    app->awake = true;
    app->page = PK_PAGE_SETTINGS;
    app->settings_quick_mode = true;
    g_writes = 0;
}

static void tap_short(pk_app_t *app, pk_ms_t t)
{
    pk_keys_dot_press(app, t);
    pk_keys_dot_release(app, t + 50);
    pk_app_tick(app, t + 50 + PK_DOUBLE_MS + 1);
}

static void tap_double(pk_app_t *app, pk_ms_t t)
{
    pk_keys_dot_press(app, t);
    pk_keys_dot_release(app, t + 50);
    pk_keys_dot_press(app, t + 100);
    pk_keys_dot_release(app, t + 150);
}

static void tap_long(pk_app_t *app, pk_ms_t t)
{
    pk_keys_dot_press(app, t);
    pk_app_tick(app, t + PK_MAIN_HOLD_MS + 1);
    pk_keys_dot_release(app, t + PK_MAIN_HOLD_MS + 50);
}

static void test_short_act_and_double_back(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.caps.sentry_known = true;
    app.caps.sentry_armed = false;
    app.focus = PK_SETTING_SENTRY;
    tap_short(&app, 2000);
    expect(g_writes == 1 && g_last_action == PK_ACTION_SET_SENTRY,
           "short tap executes a normal row");

    setup(&app, &ad);
    app.focus = PK_SETTING_SENTRY;
    g_writes = 0;
    tap_double(&app, 2000);
    expect(g_writes == 0 && app.page == PK_PAGE_MAIN,
           "double tap on settings returns home without sending");
}

static void test_dangerous_row_needs_long(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.focus = PK_SETTING_VENT;
    tap_short(&app, 2000);
    expect(g_writes == 0 && app.message == PK_MSG_HOLD_DANGER,
           "short tap on a dangerous row only explains the hold");
    expect(app.page == PK_PAGE_SETTINGS, "short tap does not change page");

    setup(&app, &ad);
    app.focus = PK_SETTING_VENT;
    tap_long(&app, 2000);
    expect(g_writes == 1 && g_last_action == PK_ACTION_GET_QUALIFICATION,
           "long press executes the dangerous row");
    expect(app.page == PK_PAGE_SETTINGS, "long press does not change page");
}

static void test_main_long_opens_settings(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    int cnt = 0;
    const pk_setting_id_t *basic = pk_app_quick_settings(&cnt);

    setup(&app, &ad);
    app.page = PK_PAGE_MAIN;
    tap_long(&app, 2000);
    expect(app.page == PK_PAGE_SETTINGS && !app.settings_quick_mode &&
           app.focus == PK_SETTING_BRIGHTNESS,
           "main long press opens BASIC 5-item settings");
    expect(cnt == 5 && basic != NULL &&
           basic[0] == PK_SETTING_BRIGHTNESS &&
           basic[1] == PK_SETTING_POWER &&
           basic[2] == PK_SETTING_LOGS &&
           basic[3] == PK_SETTING_HOTSPOT &&
           basic[4] == PK_SETTING_RESET,
           "BASIC settings table exposes exactly 5 device settings");
}

static void test_main_double_opens_status(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.page = PK_PAGE_MAIN;
    tap_double(&app, 2000);
    expect(app.page == PK_PAGE_STATUS, "main double press opens status");
}

static void test_climate_press_again_turns_off(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.caps.climate_on = PK_CAP_YES;
    app.focus = PK_SETTING_CLIMATE;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: toggle");
    expect(g_writes == 1 && g_last_action == PK_ACTION_CLIMATE_ON,
           "climate: one climate action");
    expect(app.pending.bool_enabled == PK_CAP_NO, "climate: direction is off");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "climate: ACK");
    expect(!app.has_pending && !app.busy && app.message == PK_MSG_CMD_ACCEPTED,
           "climate: ACK is terminal, no timer readback");
}

static void test_climate_unknown_state_still_turns_on(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.caps.climate_on = PK_CAP_UNKNOWN;
    app.focus = PK_SETTING_CLIMATE;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: toggle");
    expect(app.pending.bool_enabled == PK_CAP_YES,
           "climate: unknown state keeps turn-on behaviour");
}

static void test_defrost_press_again_turns_off(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.caps.preconditioning = PK_CAP_YES;
    app.focus = PK_SETTING_DEFOG;
    expect(pk_app_activate_setting(&app, PK_SETTING_DEFOG), "defrost: toggle");
    expect(g_writes == 1 && g_last_action == PK_ACTION_PRECONDITION_MAX_ON,
           "defrost: one max-preconditioning action");
    expect(app.pending.bool_enabled == PK_CAP_NO, "defrost: direction is off");
}

static void test_climate_and_defrost_off_encoding(void)
{
    pk_draft_t draft;
    uint8_t buf[64];
    size_t n;
    CarServer_Action dec;
    pb_istream_t istream;

    /* D01 upgrade: car_server.proto is syntax="proto3", where singular scalar
     * bools with default value false (0) are omitted on the canonical wire
     * (unlike the pre-S2 handwritten encoder that emitted non-canonical
     * 0x08 0x00). Verify both canonical wire bytes and official pb_decode. */
    memset(&draft, 0, sizeof(draft));
    draft.bool_enabled = PK_CAP_NO;
    n = pk_proto_encode_action_payload(PK_ACTION_CLIMATE_ON, &draft, NULL, buf, sizeof(buf));
    memset(&dec, 0, sizeof(dec));
    istream = pb_istream_from_buffer(buf, n);
    expect(n == 4 && buf[2] == 0x52 && buf[3] == 0x00 &&
           pb_decode(&istream, CarServer_Action_fields, &dec) &&
           dec.which_action_msg == CarServer_Action_vehicleAction_tag &&
           dec.action_msg.vehicleAction.which_vehicle_action_msg ==
               CarServer_VehicleAction_hvacAutoAction_tag &&
           dec.action_msg.vehicleAction.vehicle_action_msg.hvacAutoAction.power_on == false,
           "encoding: climate off encodes canonical proto3 power_on=false");

    draft.bool_enabled = PK_CAP_YES;
    n = pk_proto_encode_action_payload(PK_ACTION_CLIMATE_ON, &draft, NULL, buf, sizeof(buf));
    memset(&dec, 0, sizeof(dec));
    istream = pb_istream_from_buffer(buf, n);
    expect(n == 6 && buf[n - 2] == 0x08 && buf[n - 1] == 0x01 &&
           pb_decode(&istream, CarServer_Action_fields, &dec) &&
           dec.which_action_msg == CarServer_Action_vehicleAction_tag &&
           dec.action_msg.vehicleAction.which_vehicle_action_msg ==
               CarServer_VehicleAction_hvacAutoAction_tag &&
           dec.action_msg.vehicleAction.vehicle_action_msg.hvacAutoAction.power_on == true,
           "encoding: climate on writes power_on=1");

    draft.bool_enabled = PK_CAP_NO;
    n = pk_proto_encode_action_payload(PK_ACTION_PRECONDITION_MAX_ON, &draft, NULL,
                                       buf, sizeof(buf));
    memset(&dec, 0, sizeof(dec));
    istream = pb_istream_from_buffer(buf, n);
    expect(n == 4 && buf[2] == 0x62 && buf[3] == 0x00 &&
           pb_decode(&istream, CarServer_Action_fields, &dec) &&
           dec.which_action_msg == CarServer_Action_vehicleAction_tag &&
           dec.action_msg.vehicleAction.which_vehicle_action_msg ==
               CarServer_VehicleAction_hvacSetPreconditioningMaxAction_tag &&
           dec.action_msg.vehicleAction.vehicle_action_msg.hvacSetPreconditioningMaxAction.on == false &&
           dec.action_msg.vehicleAction.vehicle_action_msg.hvacSetPreconditioningMaxAction.manual_override == false,
           "encoding: defrost off encodes canonical proto3 on=false");
}

static void test_ack_updates_toggle_direction(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.caps.climate_on = PK_CAP_YES;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: toggle off");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "climate: ACK");
    expect(app.caps.climate_on == PK_CAP_NO, "climate: ACK learns the off state");
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: toggle back");
    expect(app.pending.bool_enabled == PK_CAP_YES,
           "climate: next press goes the other way without a readback");

    setup(&app, &ad);
    app.caps.sentry_known = true;
    app.caps.sentry_armed = false;
    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY), "sentry: enable");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "sentry: ACK");
    expect(app.caps.sentry_armed, "sentry: ACK learns enabled");
    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY), "sentry: disable");
    expect(app.pending.bool_enabled == PK_CAP_NO,
           "sentry: next press disables without a readback");
}

static void test_basic_settings_and_car_list_convergence(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    int items[8];
    int n, i;

    setup(&app, &ad);
    app.page = PK_PAGE_SETTINGS;
    app.focus = PK_SETTING_BRIGHTNESS;

    /* Arrow navigation on 08 settings cycles strictly through the 5 BASIC items */
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_POWER, "settings arrow 1 -> POWER");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_LOGS, "settings arrow 2 -> LOGS");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_HOTSPOT, "settings arrow 3 -> HOTSPOT");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_RESET, "settings arrow 4 -> RESET");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_BRIGHTNESS, "settings arrow 5 wraps -> BRIGHTNESS");
    pk_app_settings_arrow(&app, -1);
    expect(app.focus == PK_SETTING_RESET, "settings arrow -1 wraps -> RESET");

    /* 06 cars list items never include PK_LIST_QUICK */
    n = pk_app_list_items(&app, items, 8);
    expect(n > 0, "car list has items");
    for (i = 0; i < n; i++) {
        expect(items[i] != PK_LIST_QUICK, "car list does not expose PK_LIST_QUICK");
    }
}

int main(void)
{
    test_short_act_and_double_back();
    test_dangerous_row_needs_long();
    test_main_long_opens_settings();
    test_main_double_opens_status();
    test_climate_press_again_turns_off();
    test_climate_unknown_state_still_turns_on();
    test_defrost_press_again_turns_off();
    test_climate_and_defrost_off_encoding();
    test_ack_updates_toggle_direction();
    test_basic_settings_and_car_list_convergence();

    if (g_fail) {
        fprintf(stderr, "test_settings_toggle: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_settings_toggle: ok\n");
    return 0;
}
