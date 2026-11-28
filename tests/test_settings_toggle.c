/*
 * Host tests for the gesture/confirmation contract.
 *
 * Grammar: short = act, double = back/status, long = high-consequence action.
 * Timing: 300 ms double window / 500 ms hold (40 ms DOT clean-release confirm).
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
    expect(cnt == 9 && basic != NULL &&
           basic[0] == PK_SETTING_BRIGHTNESS &&
           basic[1] == PK_SETTING_POWER &&
           basic[2] == PK_SETTING_FLASH &&
           basic[3] == PK_SETTING_HORN &&
           basic[4] == PK_SETTING_CLIMATE &&
           basic[5] == PK_SETTING_DEFOG &&
           basic[6] == PK_SETTING_LOGS &&
           basic[7] == PK_SETTING_HOTSPOT &&
           basic[8] == PK_SETTING_RESET,
           "Pro-P02 settings table exposes 9 settings including FLASH, HORN, CLIMATE, and DEFOG");
}

static void test_main_double_opens_status(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    setup(&app, &ad);
    app.page = PK_PAGE_MAIN;
    tap_double(&app, 2000);
    expect(app.page == PK_PAGE_STATUS, "main double press opens status");

    /* Connected parked user can reach PK_PAGE_CARS from PK_PAGE_STATUS via ▲ */
    (void)pk_app_button_edge(&app, 2200, PK_KEY_UP, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 2240, PK_KEY_UP, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_CARS && !app.has_pending,
           "status UP key opens PK_PAGE_CARS when not driving and cancels status pending");

    /* During driving, ▲ on PK_PAGE_STATUS is blocked and stays on PK_PAGE_STATUS */
    setup(&app, &ad);
    app.page = PK_PAGE_MAIN;
    app.moving = true;
    tap_double(&app, 3000);
    expect(app.page == PK_PAGE_STATUS, "driving main double press opens status");
    (void)pk_app_button_edge(&app, 3200, PK_KEY_UP, PK_EDGE_PRESS);
    (void)pk_app_button_edge(&app, 3240, PK_KEY_UP, PK_EDGE_RELEASE);
    expect(app.page == PK_PAGE_STATUS, "driving status UP key stays on PK_PAGE_STATUS");
}

static void test_climate_press_again_turns_off(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    /* STITCH C07: climate row is one-shot ClimateOn (no on/off toggle). */
    setup(&app, &ad);
    app.caps.climate_on = PK_CAP_YES;
    app.focus = PK_SETTING_CLIMATE;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: one-shot ClimateOn");
    expect(g_writes == 1 && g_last_action == PK_ACTION_CLIMATE_ON,
           "climate: one climate action");
    expect(app.pending.bool_enabled == PK_CAP_YES, "climate: direction is always ON");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "climate: ACK");
    expect(!app.has_pending && !app.busy && app.message == PK_MSG_CMD_ACCEPTED,
           "climate: ACK is terminal, no timer readback");
}

static void test_climate_unknown_state_still_turns_on(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    /* STITCH C07: one-shot ClimateOn does not depend on prior climate_on state. */
    setup(&app, &ad);
    app.caps.climate_on = PK_CAP_UNKNOWN;
    app.focus = PK_SETTING_CLIMATE;
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE) &&
           g_writes == 1 && g_last_action == PK_ACTION_CLIMATE_ON &&
           app.pending.bool_enabled == PK_CAP_YES,
           "climate: unknown state sends one-shot ClimateOn");
}

static void test_defrost_press_again_turns_off(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;

    /* STITCH C07: defrost row is one-shot SetPreconditioningMax(true, true). */
    setup(&app, &ad);
    app.caps.preconditioning = PK_CAP_YES;
    app.focus = PK_SETTING_DEFOG;
    expect(pk_app_activate_setting(&app, PK_SETTING_DEFOG), "defrost: one-shot preconditioning max");
    expect(g_writes == 1 && g_last_action == PK_ACTION_PRECONDITION_MAX_ON,
           "defrost: one max-preconditioning action");
    expect(app.pending.bool_enabled == PK_CAP_YES, "defrost: direction is always ON");
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
    expect(app.caps.climate_on == PK_CAP_YES,
           "climate: ACCEPTED does not optimistically flip reported state");
    app.caps.climate_on = PK_CAP_NO; /* vehicle observation updates reported state */
    expect(pk_app_activate_setting(&app, PK_SETTING_CLIMATE), "climate: toggle back");
    expect(app.pending.bool_enabled == PK_CAP_YES,
           "climate: next press follows vehicle-reported state");

    setup(&app, &ad);
    app.caps.sentry_known = true;
    app.caps.sentry_armed = false;
    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY), "sentry: enable");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "sentry: ACK");
    expect(!app.caps.sentry_armed,
           "sentry: ACCEPTED does not optimistically flip reported state");
    app.caps.sentry_armed = true; /* vehicle observation updates reported state */
    expect(pk_app_activate_setting(&app, PK_SETTING_SENTRY), "sentry: disable");
    expect(app.pending.bool_enabled == PK_CAP_NO,
           "sentry: next press follows vehicle-reported state");
}

static void test_basic_settings_and_car_list_convergence(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    int items[8];
    int n, i;
    bool has_add;

    setup(&app, &ad);
    app.page = PK_PAGE_SETTINGS;
    app.focus = PK_SETTING_BRIGHTNESS;

    /* Arrow navigation on 08 settings cycles strictly through the 9 Pro-P02 items */
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_POWER, "settings arrow 1 -> POWER");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_FLASH, "settings arrow 2 -> FLASH");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_HORN, "settings arrow 3 -> HORN");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_CLIMATE, "settings arrow 4 -> CLIMATE");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_DEFOG, "settings arrow 5 -> DEFOG");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_LOGS, "settings arrow 6 -> LOGS");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_HOTSPOT, "settings arrow 7 -> HOTSPOT");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_RESET, "settings arrow 8 -> RESET");
    pk_app_settings_arrow(&app, 1);
    expect(app.focus == PK_SETTING_BRIGHTNESS, "settings arrow 9 wraps -> BRIGHTNESS");
    pk_app_settings_arrow(&app, -1);
    expect(app.focus == PK_SETTING_RESET, "settings arrow -1 wraps -> RESET");

    /* 06 cars list items never include PK_LIST_QUICK; 1 car shows PK_LIST_ADD */
    n = pk_app_list_items(&app, items, 8);
    expect(n > 0, "car list has items");
    has_add = false;
    for (i = 0; i < n; i++) {
        expect(items[i] != PK_LIST_QUICK, "car list does not expose PK_LIST_QUICK");
        if (items[i] == PK_LIST_ADD) {
            has_add = true;
        }
    }
    expect(has_add, "1-car list exposes PK_LIST_ADD");

    /* Add 2nd and 3rd cars -> capacity reaches 3, PK_LIST_ADD disappears, enter_vin blocked */
    expect(pk_store_add_verified(&app.store, "5YJS0000000000000", PK_STORE_CRASH_NONE),
           "add 2nd car");
    expect(pk_store_add_verified(&app.store, "5YJSA1E26HF000337", PK_STORE_CRASH_NONE),
           "add 3rd car");
    expect(pk_store_count(&app.store) == 3, "store count is 3");
    n = pk_app_list_items(&app, items, 8);
    has_add = false;
    for (i = 0; i < n; i++) {
        if (items[i] == PK_LIST_ADD) {
            has_add = true;
        }
    }
    expect(!has_add, "3-car full list hides PK_LIST_ADD");
    expect(!pk_app_enter_vin(&app) && app.message == PK_MSG_FULL_OR_PROTECTED,
           "3-car full blocks VIN entry with PK_MSG_FULL_OR_PROTECTED");

    /* Delete 3rd car -> capacity drops to 2, PK_LIST_ADD reappears */
    expect(pk_app_delete_vehicle(&app, 2, false, PK_STORE_CRASH_NONE), "delete slot 2");
    expect(pk_store_count(&app.store) == 2, "store count is 2 after delete");
    n = pk_app_list_items(&app, items, 8);
    has_add = false;
    for (i = 0; i < n; i++) {
        if (items[i] == PK_LIST_ADD) {
            has_add = true;
        }
    }
    expect(has_add, "2-car list restores PK_LIST_ADD");

    /* D07/D09 on BASIC 4 core actions: moving blocks UNLOCK/FRUNK/TRUNK, allows LOCK */
    app.page = PK_PAGE_MAIN;
    app.moving = true;
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(!pk_app_commit(&app, &intent, 3000), "D07/D09: moving blocks UNLOCK");
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(!pk_app_commit(&app, &intent, 3000), "D07/D09: moving blocks FRUNK");
    intent = pk_app_make_intent(&app, PK_ACTION_TRUNK_MOVE);
    expect(!pk_app_commit(&app, &intent, 3000), "D07/D09: moving blocks TRUNK");
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, 3000), "D09: moving allows LOCK");
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
