/*
 * S3 orchestrator policy: D07-D09 and N07-N12.
 * Host-only. A passing run is not a vehicle result.
 */
#include "pk_app.h"
#include "pk_app_internal.h"
#include "pk_ble_adapter.h"
#include "pk_plan.h"
#include "pk_policy.h"
#include "pk_protocol.h"
#include "pk_risk.h"

#include "key_guard.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_writes;
static int g_wakes;
static pk_action_t g_last_action;
static pk_err_t g_write_rc;

static void expect(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static const char *VIN_A = "5YJ3E1EA1PF000001";
static const char *VIN_B = "5YJS0000000000000";

static pk_err_t write_once(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    g_writes++;
    g_last_action = frame ? frame->action : PK_ACTION_NONE;
    if (frame && frame->action == PK_ACTION_VCSEC_WAKE_ONCE) {
        g_wakes++;
    }
    return g_write_rc;
}

static pk_err_t connect_once(void *ctx, int slot, pk_ms_t budget)
{
    (void)ctx;
    (void)slot;
    (void)budget;
    return PK_OK;
}

static pk_err_t disconnect_once(void *ctx, int slot)
{
    (void)ctx;
    (void)slot;
    return PK_OK;
}

static pk_err_t cancel_once(void *ctx, pk_id_t txn)
{
    (void)ctx;
    (void)txn;
    return PK_OK;
}

static bool has_session(void *ctx, int slot, pk_domain_t domain)
{
    (void)ctx;
    (void)slot;
    (void)domain;
    return true;
}

static void attach(pk_app_t *app, pk_ble_adapter_t *ad)
{
    pk_ble_adapter_init(ad);
    ad->radio.write_once = write_once;
    ad->radio.connect_once = connect_once;
    ad->radio.disconnect = disconnect_once;
    ad->radio.cancel = cancel_once;
    ad->radio.has_session = has_session;
    pk_ble_adapter_attach(app, ad);
}

static int add_and_select(pk_app_t *app, const char *vin)
{
    int slot;
    if (!pk_store_add_verified(&app->store, vin, PK_STORE_CRASH_NONE)) {
        return -1;
    }
    slot = pk_store_find_vin(&app->store, vin);
    if (slot < 0 || !pk_app_select_vehicle(app, slot)) {
        return -1;
    }
    return slot;
}

static void boot(pk_app_t *app, pk_ble_adapter_t *ad)
{
    g_writes = 0;
    g_wakes = 0;
    g_last_action = PK_ACTION_NONE;
    g_write_rc = PK_OK;
    pk_app_init(app, PK_VEHICLE_NONE, true);
    attach(app, ad);
    expect(add_and_select(app, VIN_A) >= 0, "boot: select car");
    pk_app_set_connected(app, true);
    pk_app_tick(app, 1000);
    app->awake = true;
}

static void tend(pk_app_t *app)
{
    app->moving = true;
    app->motion = PK_MOTION_TENDENCY;
    app->motion_until = app->now + PK_PARK_FRESH_MS;
}

static pk_qualify_sample_t sample_for(const pk_app_t *app, pk_shift_t shift)
{
    pk_qualify_sample_t s;
    memset(&s, 0, sizeof(s));
    s.request_id = app->pending.id;
    s.scope = app->pending.scope;
    s.shift = shift;
    s.sample_present = true;
    s.sample = (int64_t)app->now;
    s.clock_verified = true;
    s.adapter_verified = true;
    return s;
}

static void test_frozen_timing(void)
{
    expect(PK_DOUBLE_MS == 300u && PK_MAIN_HOLD_MS == 500u &&
           PK_DANGER_HOLD_MS == 500u && PK_BSP_BTN_CONFIRM_MS == 40u &&
           PK_BSP_BTN_HOLD_MS == 120u && PK_BTN_DOT_RELEASE_MS == 40u &&
           PK_PARK_FRESH_MS == 1000u && PK_PARK_LEASE_MS == 60000u &&
           PK_LOCK_VERIFY_MS == 3000u && PK_STUCK_KEY_MS == 10000u,
           "frozen timing constants");
    expect((key_stuck_loop_policy(KEY_STUCK_RELEASED) & KEY_STUCK_POLICY_DIRECT_SLEEP) == 0 &&
           (key_stuck_loop_policy(KEY_STUCK_ENTER) & KEY_STUCK_POLICY_PUMP) != 0,
           "stuck release uses the veto");
}

static void test_lock_disconnect_is_not_a_second_lock(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    int writes;

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N07: lock commits once");
    expect(app.message == PK_MSG_SENT_WAIT && app.has_pending, "N07: delivered is waiting");
    writes = g_writes;
    pk_app_set_connected(&app, false);
    expect(!app.has_pending && app.message == PK_MSG_LOCK_UNCONFIRMED,
           "N07: drop is unconfirmed");
    expect(app.motion == PK_MOTION_UNKNOWN && !app.moving, "N11: drop is not stopped");
    pk_app_set_connected(&app, true);
    expect(!app.has_pending && g_writes == writes && app.message == PK_MSG_RECONNECTED,
           "N07: reconnect does not send a second LOCK");
}

static void test_cancel_switch_and_wait(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_id_t id;
    pk_ms_t deadline;
    int writes;

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N07: lock for cancel");
    writes = g_writes;
    pk_app_exit_wait(&app);
    pk_app_tick(&app, app.now + 50);
    expect(!app.has_pending && g_writes == writes, "N07: cancel does not resend");

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N07: lock for wait");
    id = app.pending.id;
    deadline = app.pending.deadline;
    expect(!pk_app_command_result(&app, id, PK_CMD_RESULT_WAIT, true), "N07: WAIT is not terminal");
    expect(app.has_pending && app.pending.deadline == deadline, "N07: WAIT does not renew");
    pk_app_set_connected(&app, false);
    expect(!pk_app_command_result(&app, id, PK_CMD_RESULT_ACCEPTED, true),
           "N07: old receipt does not complete after disconnect");

    boot(&app, &ad);
    expect(pk_store_add_verified(&app.store, VIN_B, PK_STORE_CRASH_NONE), "N12: store second car");
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N12: lock on current car");
    writes = g_writes;
    id = app.pending.id;
    pk_app_set_connected(&app, false);
    expect(pk_app_select_vehicle(&app, pk_store_find_vin(&app.store, VIN_B)), "N12: switch car");
    expect(g_writes == writes && app.vehicle != 0, "N12: switch does not send the old LOCK");
    expect(!pk_app_command_result(&app, id, PK_CMD_RESULT_ACCEPTED, true),
           "N12: old receipt is not a new write");
}

static void test_lock_verify_failures(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_lock_packet_t pkt;
    int wakes;

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N08: lock");
    wakes = g_wakes;
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "N08: verify starts");
    expect(app.pending.kind == PK_PENDING_LOCK_VERIFY && !app.pending.wake &&
           g_last_action == PK_ACTION_VCSEC_GET_STATUS_ONCE && g_wakes == wakes,
           "N08: verify is one VCSEC read and never a wake");
    memset(&pkt, 0, sizeof(pkt));
    pkt.request_id = app.pending.id;
    pkt.authenticated = true;
    pkt.correlated = true;
    pkt.adapter_verified = true;
    pkt.complete = true;
    pkt.lock = PK_LOCK_READ_UNKNOWN;
    expect(pk_app_lock_packet(&app, &pkt), "N08: default/unknown lock is a packet");
    expect(app.message == PK_MSG_LOCK_UNCONFIRMED, "N08: unknown lock is unconfirmed");

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N08: lock for timeout");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "N08: verify pending");
    wakes = g_wakes;
    pk_app_tick(&app, app.pending.deadline + 1);
    expect(!app.has_pending && app.message == PK_MSG_LOCK_UNCONFIRMED && g_wakes == wakes,
           "N08: past 3s is unconfirmed and does not wake or reconnect-replay");

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N08: lock for disconnect");
    expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
           "N08: verify before drop");
    wakes = g_writes;
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    expect(g_writes == wakes && !app.has_pending, "N08: disconnect does not finish verify by reconnect");

    /* The post-write VCSEC read is its own 3000 ms window. It is not clipped
     * to the command total, and it does not wake or reconnect. */
    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N08: lock for unclipped verify");
    expect(app.intent_deadline == app.now + PK_COMMAND_WAIT_MS, "N08: lock total is the command budget");
    {
        pk_ms_t command_total = app.intent_deadline;
        app.now = command_total - 100u;
        wakes = g_wakes;
        expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
               "N08: verify starts inside the command total");
        expect(app.pending.kind == PK_PENDING_LOCK_VERIFY && !app.pending.wake &&
               app.pending.deadline == app.now + PK_LOCK_VERIFY_MS &&
               app.pending.deadline > command_total && g_wakes == wakes,
               "N08: lock verify keeps a separate 3000 ms window");
    }

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, app.now), "N08: unlock for unclipped verify");
    {
        pk_ms_t command_total = app.intent_deadline;
        app.now = command_total - 100u;
        wakes = g_wakes;
        expect(pk_app_command_result(&app, app.pending.id, PK_CMD_RESULT_ACCEPTED, true),
               "N08: unlock verify starts inside the command total");
        expect(app.pending.kind == PK_PENDING_UNLOCK_VERIFY &&
               app.pending.action == PK_ACTION_VCSEC_GET_STATUS_ONCE &&
               app.pending.deadline == app.now + PK_LOCK_VERIFY_MS &&
               app.pending.deadline > command_total && g_wakes == wakes,
               "N08: unlock verify keeps a separate 3000 ms window");
    }
}

static void test_wake_budget(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    int wakes;

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.pending.kind == PK_PENDING_WAKE_THEN, "N09: one wake to view");
    app.pending.qphase = 1;
    app.pending.deadline = app.now;
    pk_app_tick(&app, app.now);
    expect(g_wakes == 1 && app.pending.kind == PK_PENDING_STATUS, "N09: settle reads, it does not wake again");
    wakes = g_wakes;
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == wakes && app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE,
           "N09: status re-entry does not buy another wake");
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == wakes, "N09: reconnect does not reset the status wake budget");
}

static void test_qualification_and_motion(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_qualify_sample_t s;
    pk_intent_t intent;

    boot(&app, &ad);
    expect(pk_app_request_qualification(&app, false, false), "N11: read");
    s = sample_for(&app, PK_SHIFT_P);
    expect(!pk_app_qualification_result(&app, &s), "N11: missing speed is not park");
    expect(!pk_app_parked(&app) && app.motion == PK_MOTION_UNKNOWN, "N11: missing speed is unknown");

    expect(pk_app_request_qualification(&app, false, false), "N11: read zero");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    expect(pk_app_qualification_result(&app, &s) && pk_app_parked(&app), "N11: P plus one legal zero parks");
    expect(!pk_app_known_motion(&app), "N11: park is not a motion tendency");

    expect(pk_app_request_qualification(&app, false, false), "N11: read conflict");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    s.speed_float_present = true;
    s.speed_float = 4;
    expect(!pk_app_qualification_result(&app, &s) && !pk_app_parked(&app), "N11: 0 spliced with non-zero is not park");
    expect(pk_app_known_motion(&app), "N11: conflict is a motion tendency");

    expect(pk_app_request_qualification(&app, false, false), "N11: read D");
    s = sample_for(&app, PK_SHIFT_D);
    expect(!pk_app_qualification_result(&app, &s) && pk_app_known_motion(&app), "N11: D is tendency, not park");

    expect(pk_app_request_qualification(&app, false, false), "N11: untrusted D");
    s = sample_for(&app, PK_SHIFT_D);
    s.adapter_verified = false;
    expect(!pk_app_qualification_result(&app, &s) && !pk_app_known_motion(&app),
           "N11: untrusted D does not become motion or park");

    boot(&app, &ad);
    expect(pk_app_request_qualification(&app, false, false), "N11: fresh park");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    pk_app_qualification_result(&app, &s);
    tend(&app);
    app.motion_until = app.now;
    pk_app_tick(&app, app.now);
    expect(!pk_app_known_motion(&app) && app.motion == PK_MOTION_UNKNOWN,
           "N11: expired tendency is unknown, not stopped");
    expect(pk_app_parked(&app), "N11: the display lease outlives the 1s motion window");
    app.grant.received = app.now - (PK_PARK_FRESH_MS + 1u);

    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    g_writes = 0;
    expect(pk_app_commit(&app, &intent, app.now), "D07: unknown motion does not send a closure");
    expect(app.pending.kind == PK_PENDING_QUALIFICATION && g_last_action == PK_ACTION_GET_QUALIFICATION,
           "D07: closure stays unqualified");
    s = sample_for(&app, PK_SHIFT_P);
    expect(!pk_app_qualification_result(&app, &s), "D07: missing speed does not release the closure");
    expect(!app.has_pending && g_last_action != PK_ACTION_FRUNK_OPEN, "D07: no closure write");
}

static void test_driving_matrix(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    boot(&app, &ad);
    tend(&app);
    app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_YES;
    app.caps.action[PK_ACTION_SET_SENTRY] = PK_CAP_YES;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now) && app.pending.action == PK_ACTION_CLIMATE_ON,
           "D09: climate is allowed while moving");
    pk_app_exit_wait(&app);
    intent = pk_app_make_intent(&app, PK_ACTION_SET_SENTRY);
    intent.bool_enabled = PK_CAP_YES;
    expect(!pk_app_commit(&app, &intent, app.now) && app.message == PK_MSG_MOVING,
           "D09: sentry is blocked while moving");
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(!pk_app_commit(&app, &intent, app.now) && !pk_app_can_vcsec_unlock(&app),
           "D07: known motion refuses unlock");
    intent = pk_app_make_intent(&app, PK_ACTION_FLASH);
    expect(!pk_app_commit(&app, &intent, app.now), "D09: flash is blocked while moving");
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now) && app.pending.action == PK_ACTION_LOCK,
           "D09: LOCK is an explicit send while moving");
    expect(!pk_plan_replay_allowed(PK_ACTION_LOCK) && !pk_risk_replay_allowed(PK_RISK_SAFE),
           "D09: LOCK is not auto-replayed");
    pk_app_exit_wait(&app);
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(!pk_app_commit(&app, &intent, app.now), "D09: closure is blocked while moving");

    pk_app_motion_unknown(&app);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    app.vcsec_lock_state = PK_LOCK_READ_LOCKED;
    expect(pk_app_primary(&app) == PK_ACTION_UNLOCK, "D07: locked and not moving may show UNLOCK");
    app.vcsec_lock_state = PK_LOCK_READ_UNKNOWN;
    app.vcsec_asleep = true;
    expect(pk_app_primary(&app) == PK_ACTION_LOCK, "D07: asleep alone does not select UNLOCK");
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(pk_app_commit(&app, &intent, app.now) && !pk_app_parked(&app),
           "D07: motion unknown allows unlock and is not park proof");
    pk_app_exit_wait(&app);
    intent = pk_app_make_intent(&app, PK_ACTION_SET_SENTRY);
    intent.bool_enabled = PK_CAP_YES;
    expect(pk_app_commit(&app, &intent, app.now), "D09: sentry is allowed when motion is unknown");
}

static void test_send_map(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;

    boot(&app, &ad);
    g_write_rc = PK_ERR_NOT_WIRED;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(!pk_app_commit(&app, &intent, app.now) && !app.has_pending &&
           app.message == PK_MSG_CANCELLED,
           "N07: unsent lock is cancelled");

    g_write_rc = PK_ERR_UNKNOWN;
    intent = pk_app_make_intent(&app, PK_ACTION_LOCK);
    expect(pk_app_commit(&app, &intent, app.now) && !app.has_pending &&
           app.message == PK_MSG_LOCK_UNCONFIRMED,
           "N07: no completion channel is unconfirmed");
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    expect(!app.has_pending, "N07: unknown lock is not retried on reconnect");
}

static void test_invalid_speed_motion(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_qualify_sample_t s;
    pk_intent_t intent;

    boot(&app, &ad);
    expect(pk_app_request_qualification(&app, false, false), "speed: D read");
    s = sample_for(&app, PK_SHIFT_D);
    s.speed_float_present = true;
    s.speed_float = -1;
    s.speed_invalid = true;
    expect(!pk_app_qualification_result(&app, &s) && pk_app_known_motion(&app) &&
           !pk_app_parked(&app),
           "D07: D plus invalid speed is motion and not park");
    intent = pk_app_make_intent(&app, PK_ACTION_UNLOCK);
    expect(!pk_app_commit(&app, &intent, app.now) && g_last_action != PK_ACTION_UNLOCK,
           "D07: D plus invalid speed does not unlock");

    boot(&app, &ad);
    expect(pk_app_request_qualification(&app, false, false), "speed: finite read");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 5;
    s.speed_float_present = true;
    s.speed_float = -1;
    s.speed_invalid = true;
    expect(!pk_app_qualification_result(&app, &s) && pk_app_known_motion(&app) &&
           !pk_app_parked(&app),
           "D07: finite 5 mph beside an illegal speed is motion and not park");
    intent = pk_app_make_intent(&app, PK_ACTION_FLASH);
    expect(!pk_app_commit(&app, &intent, app.now), "D09: that motion blocks flash");

    boot(&app, &ad);
    expect(pk_app_request_qualification(&app, false, false), "speed: nan park");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    s.speed_float_present = true;
    s.speed_float = -1;
    s.speed_invalid = true;
    expect(!pk_app_qualification_result(&app, &s) && !pk_app_known_motion(&app) &&
           !pk_app_parked(&app),
           "D07: P plus 0 plus NaN is not motion and not park");
}

static void test_busy_keeps_frozen_intent(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t first;
    pk_intent_t second;
    pk_qualify_sample_t s;
    int writes;
    bool spent;
    int owner;

    boot(&app, &ad);
    first = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &first, app.now), "busy: frunk check starts");
    expect(app.has_deferred && app.busy && app.pending.kind == PK_PENDING_QUALIFICATION &&
           app.deferred.action == PK_ACTION_FRUNK_OPEN,
           "busy: original frunk is frozen");
    writes = g_writes;
    spent = app.wake_spent;
    owner = app.wake_owner;
    second = pk_app_make_intent(&app, PK_ACTION_TRUNK_MOVE);
    expect(!pk_app_commit(&app, &second, app.now) && app.message == PK_MSG_BUSY_NO_QUEUE,
           "busy: trunk overlap is rejected");
    expect(app.has_deferred && app.deferred.action == PK_ACTION_FRUNK_OPEN &&
           app.wake_spent == spent && app.wake_owner == owner &&
           app.pending.kind == PK_PENDING_QUALIFICATION,
           "busy: rejection keeps deferred intent and wake budget");
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    expect(pk_app_qualification_result(&app, &s), "busy: P plus 0 qualifies");
    expect(g_last_action == PK_ACTION_FRUNK_OPEN && g_writes == writes + 1,
           "busy: original frunk submits once");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    first = pk_app_make_intent(&app, PK_ACTION_SET_SENTRY);
    first.bool_enabled = PK_CAP_YES;
    expect(pk_app_commit(&app, &first, app.now) && app.has_deferred &&
           app.deferred.bool_enabled == PK_CAP_YES && app.wake_spent,
           "busy: sentry params freeze behind one wake");
    spent = app.wake_spent;
    owner = app.wake_owner;
    second = pk_app_make_intent(&app, PK_ACTION_SET_SENTRY);
    second.bool_enabled = PK_CAP_NO;
    expect(!pk_app_commit(&app, &second, app.now), "busy: replacement sentry is rejected");
    expect(app.has_deferred && app.deferred.action == PK_ACTION_SET_SENTRY &&
           app.deferred.bool_enabled == PK_CAP_YES && app.wake_spent == spent &&
           app.wake_owner == owner,
           "busy: rejection keeps the original sentry parameter and wake owner");

    boot(&app, &ad);
    expect(pk_side_press(&app, PK_KEY_UP, app.now), "busy: side frunk arms");
    pk_side_tick(&app, app.now + pk_action_hold_ms(PK_ACTION_FRUNK_OPEN));
    expect(app.has_deferred && app.deferred.action == PK_ACTION_FRUNK_OPEN,
           "busy: side hold freezes frunk");
    pk_side_release(&app, PK_KEY_UP, app.now);
    writes = g_writes;
    (void)pk_side_press(&app, PK_KEY_DOWN, app.now);
    pk_side_tick(&app, app.now + pk_action_hold_ms(PK_ACTION_TRUNK_MOVE));
    expect(app.has_deferred && app.deferred.action == PK_ACTION_FRUNK_OPEN &&
           g_writes == writes,
           "busy: side trunk does not clear the frozen frunk");
}

static void test_intent_deadline_and_cancel(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_qualify_sample_t s;
    pk_ms_t total;
    pk_ms_t started;
    int writes;

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "deadline: climate wake");
    started = app.now;
    total = app.pending.deadline;
    expect(total == started + PK_WAKE_THEN_READ_MS, "deadline: wake stamps the original total");
    app.now = total - 1u;
    app.pending.qphase = 1;
    app.pending.deadline = app.now;
    pk_app_tick(&app, app.now);
    expect(g_last_action == PK_ACTION_CLIMATE_ON && app.has_pending &&
           app.pending.deadline <= total,
           "deadline: late settle does not extend past the original total");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "deadline: climate for expiry");
    total = app.pending.deadline;
    writes = g_writes;
    app.now = total;
    app.pending.qphase = 1;
    app.pending.deadline = total;
    pk_app_tick(&app, app.now);
    expect(g_writes == writes && g_last_action != PK_ACTION_CLIMATE_ON && !app.has_deferred,
           "deadline: at the original total the command is not submitted");

    boot(&app, &ad);
    started = app.now;
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now), "deadline: frunk check");
    total = app.pending.deadline;
    expect(total == started + PK_QUALIFICATION_READ_MS, "deadline: check stamps its original total");
    app.now = started + 1000u;
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    expect(pk_app_qualification_result(&app, &s), "deadline: P plus 0 inside the total");
    expect(g_last_action == PK_ACTION_FRUNK_OPEN && app.pending.deadline <= total,
           "deadline: qualification follow-up does not extend the original total");

    boot(&app, &ad);
    started = app.now;
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now), "deadline: frunk for late result");
    total = app.pending.deadline;
    writes = g_writes;
    app.now = total;
    s = sample_for(&app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    expect(!pk_app_qualification_result(&app, &s) && g_writes == writes,
           "deadline: a result at the original total does not submit");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "cancel: climate wake");
    writes = g_writes;
    pk_app_exit_wait(&app);
    pk_app_tick(&app, app.now + 50u);
    expect(!app.has_deferred && g_writes == writes && g_last_action != PK_ACTION_CLIMATE_ON,
           "cancel: exit does not submit the frozen climate");

    boot(&app, &ad);
    expect(pk_store_add_verified(&app.store, VIN_B, PK_STORE_CRASH_NONE), "switch: second car");
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now), "switch: frunk check");
    writes = g_writes;
    expect(!pk_app_select_vehicle(&app, pk_store_find_vin(&app.store, VIN_B)) &&
           app.has_deferred && app.deferred.action == PK_ACTION_FRUNK_OPEN &&
           g_writes == writes,
           "switch: a busy check refuses the other car and does not submit");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "reentry: climate wake");
    writes = g_writes;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(app.has_deferred && app.deferred.action == PK_ACTION_CLIMATE_ON &&
           app.wake_spent && app.wake_owner == PK_WAKE_OWNER_COMMAND &&
           app.pending.kind == PK_PENDING_WAKE_THEN && g_writes == writes,
           "reentry: status does not reset the command wake or submit climate");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "drop: climate wake");
    writes = g_writes;
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    expect(!app.has_deferred && g_writes == writes && g_last_action != PK_ACTION_CLIMATE_ON,
           "drop: reconnect does not submit the old climate");
}

static void spend_status_wake(pk_app_t *app, pk_ble_adapter_t *ad)
{
    boot(app, ad);
    app->vcsec_status_valid = true;
    app->vcsec_asleep = true;
    pk_app_open_status(app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app->pending.kind == PK_PENDING_WAKE_THEN &&
           app->wake_owner == PK_WAKE_OWNER_STATUS && app->wake_spent,
           "guard setup: one status wake");
    app->pending.qphase = 1;
    app->pending.deadline = app->now;
    pk_app_tick(app, app->now);
    pk_app_exit_status(app);
    expect(g_wakes == 1 && !app->has_pending && !app->has_deferred &&
           app->wake_owner == PK_WAKE_OWNER_STATUS && app->wake_spent &&
           app->page != PK_PAGE_STATUS,
           "guard setup: leaving status keeps the spent wake");
}

static void grant_park(pk_app_t *app)
{
    pk_qualify_sample_t s;

    expect(pk_app_request_qualification(app, false, false), "guard setup: park read");
    s = sample_for(app, PK_SHIFT_P);
    s.speed_present = true;
    s.speed = 0;
    expect(pk_app_qualification_result(app, &s) && pk_app_park_fresh(app),
           "guard setup: fresh park");
}

static void expect_guard_keeps(pk_app_t *app, pk_action_t action, pk_msg_id_t msg,
                               uint8_t owner, bool spent, pk_ms_t deadline,
                               int wakes, const char *name)
{
    pk_intent_t intent = pk_app_make_intent(app, action);
    int writes = g_writes;
    bool accepted = pk_app_commit(app, &intent, app->now);

    expect(!accepted && app->message == msg && app->wake_owner == owner &&
           app->wake_spent == spent && app->intent_deadline == deadline &&
           !app->has_deferred && g_wakes == wakes && g_writes == writes, name);
}

static void test_guard_rejection_keeps_wake(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_intent_t intent;
    pk_ms_t deadline;
    int wakes;

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    tend(&app);
    expect_guard_keeps(&app, PK_ACTION_UNLOCK, PK_MSG_MOVING,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: moving UNLOCK does not reset a spent status wake");
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE,
           "guard: moving UNLOCK does not create another status wake");

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    tend(&app);
    expect_guard_keeps(&app, PK_ACTION_FRUNK_OPEN, PK_MSG_MOVING,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: moving closure does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    app.paired = false;
    expect_guard_keeps(&app, PK_ACTION_LOCK, PK_MSG_NOT_PAIRED,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: unpaired LOCK does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_NO;
    expect_guard_keeps(&app, PK_ACTION_CLIMATE_ON, PK_MSG_CAPABILITY,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: capability refusal does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    app.caps.climate_on = PK_CAP_NO;
    app.caps.keeper_on = PK_CAP_NO;
    expect_guard_keeps(&app, PK_ACTION_SET_SEAT_HEAT, PK_MSG_NEED_CLIMATE,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: seat-heat refusal does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    grant_park(&app);
    expect(app.wake_owner == PK_WAKE_OWNER_STATUS && app.wake_spent && g_wakes == 1,
           "guard setup: park read keeps the status wake");
    deadline = app.intent_deadline;
    app.caps.cable_connected = PK_CAP_YES;
    expect_guard_keeps(&app, PK_ACTION_CHARGE_PORT_CLOSE, PK_MSG_CABLE,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: cable refusal does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    grant_park(&app);
    deadline = app.intent_deadline;
    app.caps.car_wash = PK_CAP_YES;
    expect_guard_keeps(&app, PK_ACTION_CHARGE_PORT_OPEN, PK_MSG_CAR_WASH,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: car-wash refusal does not reset a spent status wake");

    spend_status_wake(&app, &ad);
    pk_app_set_connected(&app, false);
    expect(app.wake_owner == PK_WAKE_OWNER_STATUS && app.wake_spent && g_wakes == 1,
           "guard setup: disconnect keeps the spent status wake");
    deadline = app.intent_deadline;
    expect_guard_keeps(&app, PK_ACTION_LOCK, PK_MSG_RECONNECT_FIRST,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: reconnect refusal does not reset a spent status wake");
    pk_app_set_connected(&app, true);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE,
           "guard: reconnect refusal does not create another status wake");

    spend_status_wake(&app, &ad);
    deadline = app.intent_deadline;
    app.busy = true;
    expect_guard_keeps(&app, PK_ACTION_UNLOCK, PK_MSG_BUSY_NO_QUEUE,
                       PK_WAKE_OWNER_STATUS, true, deadline, 1,
                       "guard: busy refusal does not reset a spent status wake");

    boot(&app, &ad);
    tend(&app);
    expect_guard_keeps(&app, PK_ACTION_UNLOCK, PK_MSG_MOVING,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: moving UNLOCK does not create a wake budget");

    boot(&app, &ad);
    app.paired = false;
    expect_guard_keeps(&app, PK_ACTION_LOCK, PK_MSG_NOT_PAIRED,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: unpaired LOCK does not create a wake budget");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    app.caps.action[PK_ACTION_CLIMATE_ON] = PK_CAP_NO;
    expect_guard_keeps(&app, PK_ACTION_CLIMATE_ON, PK_MSG_CAPABILITY,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: capability refusal does not create a wake budget");

    boot(&app, &ad);
    app.caps.climate_on = PK_CAP_NO;
    app.caps.keeper_on = PK_CAP_NO;
    expect_guard_keeps(&app, PK_ACTION_SET_SEAT_HEAT, PK_MSG_NEED_CLIMATE,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: seat-heat refusal does not create a wake budget");

    boot(&app, &ad);
    grant_park(&app);
    app.caps.cable_connected = PK_CAP_YES;
    expect_guard_keeps(&app, PK_ACTION_CHARGE_PORT_CLOSE, PK_MSG_CABLE,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: cable refusal does not create a wake budget");

    boot(&app, &ad);
    grant_park(&app);
    app.caps.car_wash = PK_CAP_YES;
    expect_guard_keeps(&app, PK_ACTION_CHARGE_PORT_OPEN, PK_MSG_CAR_WASH,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: car-wash refusal does not create a wake budget");

    boot(&app, &ad);
    pk_app_set_connected(&app, false);
    expect_guard_keeps(&app, PK_ACTION_UNLOCK, PK_MSG_RECONNECT_FIRST,
                       PK_WAKE_OWNER_NONE, false, 0, 0,
                       "guard: reconnect refusal does not create a wake budget");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now) && app.has_deferred &&
           app.intent_deadline != 0, "cleanup: climate wake is open");
    wakes = g_writes;
    pk_app_tx_failed(&app, app.pending.id, PK_TX_NOT_SENT);
    pk_app_tick(&app, app.now + 50u);
    expect(!app.has_deferred && app.intent_deadline == 0 && !app.has_pending &&
           g_writes == wakes && g_last_action != PK_ACTION_CLIMATE_ON,
           "cleanup: tx failure drops the gesture and does not resubmit");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    intent = pk_app_make_intent(&app, PK_ACTION_CLIMATE_ON);
    expect(pk_app_commit(&app, &intent, app.now), "cleanup: climate for wake expiry");
    wakes = g_writes;
    pk_app_tick(&app, app.intent_deadline);
    expect(!app.has_deferred && app.intent_deadline == 0 && g_writes == wakes &&
           g_last_action != PK_ACTION_CLIMATE_ON && app.message == PK_MSG_UNQUALIFIED,
           "cleanup: wake expiry drops the gesture and does not resubmit");

    boot(&app, &ad);
    intent = pk_app_make_intent(&app, PK_ACTION_FRUNK_OPEN);
    expect(pk_app_commit(&app, &intent, app.now) && app.has_deferred,
           "cleanup: frunk check is frozen");
    wakes = g_writes;
    pk_app_protocol_fault(&app, PK_PROTO_FAULT_TIMEOUT, false);
    pk_app_tick(&app, app.now + 50u);
    expect(!app.has_deferred && app.intent_deadline == 0 && !app.has_pending &&
           g_writes == wakes && g_last_action != PK_ACTION_FRUNK_OPEN,
           "cleanup: protocol fault drops the gesture and does not resubmit");
}

static void test_status_wake_deadline(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_ms_t total;
    pk_ms_t started;
    int wakes;

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    started = app.now;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    total = app.pending.deadline;
    expect(g_wakes == 1 && app.pending.kind == PK_PENDING_WAKE_THEN &&
           total == started + PK_WAKE_THEN_READ_MS && app.intent_deadline == total,
           "status: wake freezes the original 16000 ms total");
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.pending.kind == PK_PENDING_WAKE_THEN &&
           app.pending.deadline == total && app.intent_deadline == total,
           "status: reentry during the wake does not extend the total");

    app.now = total - 500u;
    app.pending.qphase = 1;
    app.pending.deadline = app.now;
    pk_app_tick(&app, app.now);
    expect(g_wakes == 1 && app.pending.kind == PK_PENDING_STATUS &&
           app.pending.deadline <= total &&
           app.pending.deadline != app.now + PK_STATUS_READ_MS,
           "status: read continuation cannot pass the original total");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    total = app.pending.deadline;
    wakes = g_wakes;
    app.pending.qphase = 1;
    app.pending.deadline = total;
    pk_app_tick(&app, total);
    expect(g_wakes == wakes &&
           !(app.has_pending && app.pending.deadline > total) &&
           !(app.has_pending && app.pending.kind == PK_PENDING_WAKE_THEN),
           "status: expiry at the original total does not extend or wake again");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    total = app.pending.deadline;
    app.pending.qphase = 1;
    app.pending.deadline = app.now;
    pk_app_tick(&app, app.now);
    pk_app_exit_status(&app);
    app.now = total - 500u;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.wake_owner == PK_WAKE_OWNER_STATUS && app.wake_spent &&
           app.pending.kind == PK_PENDING_STATUS && app.pending.deadline <= total &&
           app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE,
           "status: reentry inside the window does not extend or wake again");

    boot(&app, &ad);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    total = app.pending.deadline;
    app.now = total - 500u;
    pk_app_set_connected(&app, false);
    pk_app_set_connected(&app, true);
    app.vcsec_status_valid = true;
    app.vcsec_asleep = true;
    pk_app_open_status(&app, PK_ORIGIN_LIST);
    expect(g_wakes == 1 && app.wake_spent && app.wake_owner == PK_WAKE_OWNER_STATUS &&
           app.pending.action != PK_ACTION_VCSEC_WAKE_ONCE &&
           (!app.has_pending || app.pending.deadline <= total),
           "status: reconnect does not extend the total or buy another wake");
}

static void test_role_and_management(void)
{
    pk_app_t app;
    pk_ble_adapter_t ad;
    pk_pair_verify_t v;
    pk_msg_id_t why = PK_MSG_EMPTY;

    boot(&app, &ad);
    expect(pk_app_management_allowed(&app, PK_MGMT_DELETE, true, &why),
           "N12: missing park does not seal delete");
    tend(&app);
    expect(!pk_app_management_allowed(&app, PK_MGMT_DELETE, true, &why) &&
           why == PK_MSG_MGMT_BUSY_OR_MOVING,
           "N12: known motion still blocks management");

    pk_app_init(&app, PK_VEHICLE_NONE, true);
    attach(&app, &ad);
    app.now = 1000;
    expect(pk_app_enter_vin(&app) && pk_app_submit_vin(&app, VIN_A), "N12: vin");
    expect(pk_app_pair_start(&app, VIN_A, true, true), "N12: pair start");
    expect(pk_app_pair_next(&app) && pk_app_pair_next(&app), "N12: reach addkey");
    expect(app.addkey_requests_driver && !app.role_is_driver && !app.role_observed,
           "N12: AddKey requests Driver and does not read it");
    expect(pk_app_pair_approve(&app, true, app.pair_id, true), "N12: approve");
    expect(pk_app_pair_next(&app), "N12: verify step");
    memset(&v, 0, sizeof(v));
    v.request_id = app.pair_id;
    v.vcsec_authenticated = true;
    v.infotainment_authenticated = true;
    v.key_matches = true;
    v.role_driver = true;
    v.identity_binding_verified = true;
    expect(pk_app_pair_verify(&app, &v), "N12: session proof saves without a role read");
    expect(app.session_vcsec_proven && app.session_infotainment_proven &&
           !app.role_observed && !app.role_is_driver && !app.identity_binding_verified,
           "N12: handshake does not hard-fill role or identity");
}

static void seed_transient_auth_state(pk_app_t *app)
{
    memset(&app->grant, 0, sizeof(app->grant));
    app->has_grant = true;
    app->grant.scope = pk_app_scope(app);
    app->grant.until = app->now + PK_PARK_LEASE_MS;
    app->grant.received = app->now;
    app->grant.proof = true;
    app->grant.parked = true;
    app->grant.speed_present = true;
    app->connected = true;
    app->auth = PK_MSG_AUTH_EXPIRED;
    app->auth_until = app->now + 60000;
    app->reconnect.active = true;
    app->addkey_requests_driver = true;
    app->role_observed = true;
    app->role_is_driver = true;
    app->identity_binding_verified = true;
    app->session_vcsec_proven = true;
    app->session_infotainment_proven = true;
}

static bool transient_auth_state_cleared(const pk_app_t *app)
{
    return app->auth == PK_MSG_AUTH_UNCONFIRMED &&
           app->auth_until == 0 &&
           !app->reconnect.active &&
           !app->addkey_requests_driver &&
           !app->role_observed &&
           !app->role_is_driver &&
           !app->identity_binding_verified &&
           !app->session_vcsec_proven &&
           !app->session_infotainment_proven;
}

static void test_auth_lifecycle_cleanup(void)
{
    static const char *VIN_C = "SYNTH000000000003";
    pk_app_t app;
    pk_app_t cold;
    pk_ble_adapter_t ad;
    pk_ms_t old_auth_until;

    /* Cold start must begin clean. */
    pk_app_init(&cold, PK_VEHICLE_NONE, false);
    expect(cold.auth_until == 0 && !cold.reconnect.active &&
           !cold.addkey_requests_driver && !cold.role_observed &&
           !cold.role_is_driver && !cold.identity_binding_verified &&
           !cold.session_vcsec_proven && !cold.session_infotainment_proven,
           "lifecycle: cold start has no transient auth/role/proof state");

    /* Switch: re-selecting current keeps state; switching to another clears it. */
    boot(&app, &ad);
    expect(pk_store_add_verified(&app.store, VIN_B, PK_STORE_CRASH_NONE) &&
           pk_store_add_verified(&app.store, VIN_C, PK_STORE_CRASH_NONE),
           "lifecycle: add B and C");
    seed_transient_auth_state(&app);
    old_auth_until = app.auth_until;
    expect(pk_app_select_vehicle(&app, 0) &&
           app.role_observed && app.session_vcsec_proven &&
           app.auth_until == old_auth_until && app.connected && app.has_grant,
           "lifecycle: reselecting current car preserves live session state");
    expect(pk_app_select_vehicle(&app, 1) && app.vehicle == 1 &&
           transient_auth_state_cleared(&app),
           "lifecycle: switch clears transient proof, role, and auth timer");
    expect(pk_app_tick(&app, old_auth_until + 1000) == PK_OK &&
           app.auth == PK_MSG_AUTH_UNCONFIRMED &&
           app.message != PK_MSG_AUTH_EXPIRED,
           "lifecycle: switch leaves no dangling auth_until timer");

    /* Delete non-current preserves current car's live state; delete current clears it. */
    expect(pk_app_select_vehicle(&app, 0) && app.vehicle == 0, "lifecycle: back to slot 0");
    seed_transient_auth_state(&app);
    old_auth_until = app.auth_until;
    expect(pk_app_delete_vehicle(&app, 1, false, PK_STORE_CRASH_NONE) &&
           app.vehicle == 0 && app.connected && app.has_grant &&
           app.role_observed && app.role_is_driver &&
           app.identity_binding_verified && app.session_vcsec_proven &&
           app.session_infotainment_proven && app.auth_until == old_auth_until,
           "lifecycle: deleting non-current car preserves current live session");
    expect(pk_app_delete_vehicle(&app, 0, false, PK_STORE_CRASH_NONE) &&
           app.vehicle == 2 && transient_auth_state_cleared(&app),
           "lifecycle: deleting current car clears transient proof, role, and auth timer");
    expect(pk_app_tick(&app, old_auth_until + 1000) == PK_OK &&
           app.auth == PK_MSG_AUTH_UNCONFIRMED &&
           app.message != PK_MSG_AUTH_EXPIRED,
           "lifecycle: delete-current leaves no dangling auth_until timer");

    /* Runtime reboot clears transient proof, role, and auth timer while keeping slot. */
    seed_transient_auth_state(&app);
    old_auth_until = app.auth_until;
    pk_app_reboot(&app);
    expect(app.vehicle == 2 && transient_auth_state_cleared(&app),
           "lifecycle: runtime reboot clears transient proof, role, and auth timer");
    expect(pk_app_tick(&app, old_auth_until + 1000) == PK_OK &&
           app.auth == PK_MSG_AUTH_UNCONFIRMED &&
           app.message != PK_MSG_AUTH_EXPIRED,
           "lifecycle: runtime reboot leaves no dangling auth_until timer");

    /* Disconnect marking auth unconfirmed also clears auth_until. */
    app.connected = true;
    app.auth = PK_MSG_AUTH_EXPIRED;
    app.auth_until = app.now + 60000;
    old_auth_until = app.auth_until;
    pk_app_set_connected(&app, false);
    expect(app.auth == PK_MSG_AUTH_UNCONFIRMED && app.auth_until == 0,
           "lifecycle: disconnect clears auth_until when auth becomes unconfirmed");
    expect(pk_app_tick(&app, old_auth_until + 1000) == PK_OK &&
           app.auth == PK_MSG_AUTH_UNCONFIRMED &&
           app.message != PK_MSG_AUTH_EXPIRED,
           "lifecycle: disconnect leaves no dangling auth_until timer");
}

int main(void)
{
    test_frozen_timing();
    test_lock_disconnect_is_not_a_second_lock();
    test_cancel_switch_and_wait();
    test_lock_verify_failures();
    test_wake_budget();
    test_qualification_and_motion();
    test_invalid_speed_motion();
    test_busy_keeps_frozen_intent();
    test_intent_deadline_and_cancel();
    test_guard_rejection_keeps_wake();
    test_status_wake_deadline();
    test_driving_matrix();
    test_send_map();
    test_role_and_management();
    test_auth_lifecycle_cleanup();
    if (g_fail) {
        fprintf(stderr, "s3 orchestrator: %d FAIL\n", g_fail);
        return 1;
    }
    printf("s3 orchestrator: PASS\n");
    return 0;
}
