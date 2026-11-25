#include "pk_app_internal.h"
#include "pk_plan.h"
#include "pk_protocol.h"

#include <string.h>

/*
 * M11: on-enter bounded serial status read, 4000 ms cap. No background
 * poll, no durable cache, no cloud. Leaving drops volatile display and
 * unfinished requests; re-entry starts a new read_id.
 *
 * Origin: list → ● back to the vehicle list; driving → ● back to the
 * driving home screen; long-● home. Driving must not enter vehicle
 * management through status.
 *
 * Each field keeps value / presence / sample time / read time / source /
 * unit evidence apart. No sample time ⇒ not "live". Stale is not current.
 * Range uses only battery_range; estimated / ideal are not mixed in.
 * Without a verified unit, show "—" and "单位待确认"; never invent km.
 * Charge-limit ETA and full-charge ETA stay separate. Cabin temp is
 * kept; pack temp is not added. Power stays "—" until a verified source.
 *
 * 待真机: range unit, power unit and sign, TPMS order and bar sampling.
 * Copy stays honest about pending units and sample timeliness.
 */

static bool status_section_usable(const pk_app_t *app, int section, uint16_t mtu)
{
    if (!app || section < 0 || section >= PK_STATUS_SECTION_COUNT) {
        return false;
    }
    if ((app->status_skip_mask | app->status_too_large_mask) &
        (uint8_t)(1u << section)) {
        return false;
    }
    return pk_plan_status_section_fits(section, mtu);
}

static int status_next_section(const pk_app_t *app, int from)
{
    uint16_t mtu = pk_ble_effective_mtu(app);
    int section;

    for (section = from; section < PK_STATUS_SECTION_COUNT; section++) {
        if (status_section_usable(app, section, mtu)) {
            return section;
        }
    }
    return -1;
}

static pk_ms_t status_section_deadline(const pk_app_t *app)
{
    return pk_app_phase_deadline(app, app->now, pk_plan_deadline(PK_PLAN_STATUS_SECTION));
}

static void status_finish_too_large(pk_app_t *app)
{
    pk_ble_release_in_flight(app);
    app->status_need_send = false;
    app->has_pending = false;
    pk_app_set_busy(app, false);
    strncpy(app->status_phase, "too_large", sizeof(app->status_phase) - 1);
    pk_app_set_msg(app, PK_MSG_RESPONSE_TOO_LARGE);
}

void pk_app_open_status(pk_app_t *app, pk_origin_t origin)
{
    pk_pending_t *p;
    int first;
    if (!app) {
        return;
    }
    /* A command intent already owns the wake budget and the wire.
     * Opening status must not clear it or buy another wake. */
    if (app->has_deferred || app->busy ||
        (app->has_pending && app->pending.kind != PK_PENDING_STATUS)) {
        return;
    }
    pk_arbiter_invalidate(&app->keys);
    app->origin = app->moving ? PK_ORIGIN_DRIVING : origin;
    if (app->page != PK_PAGE_STATUS) {
        app->status_return_page = app->page; /* gesture entry */
    }
    app->page = PK_PAGE_STATUS;
    app->field_count = 0;
    /* Known-asleep is not a reason to send RKE_ACTION_WAKE_VEHICLE.
     * Reading status is not a wake. A user-initiated wake, and the explicit
     * wake for a command that needs Infotainment, stay on their own paths. */
    if (app->vehicle == PK_VEHICLE_NONE) {
        strncpy(app->status_phase, "no_target", sizeof(app->status_phase) - 1);
        return;
    }
    if (!app->connected) {
        strncpy(app->status_phase, "offline", sizeof(app->status_phase) - 1);
        return;
    }
    strncpy(app->status_phase, "loading", sizeof(app->status_phase) - 1);
    app->serial++;
    app->status_step = 0;
    app->status_skip_mask = 0;
    app->status_need_send = false;
    first = status_next_section(app, 0);
    if (first < 0) {
        status_finish_too_large(app);
        return;
    }
    app->status_step = (uint8_t)first;
    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_STATUS;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    p->deadline = status_section_deadline(app);
    p->action = PK_ACTION_STATUS_READ;
    app->has_pending = true;
    pk_ble_release_in_flight(app);
    if (pk_ble_send_status_step(app, p->deadline) == PK_ERR_BLOCKED) {
        /* Selection and send use the same MTU in the normal path; handle a
         * change in between without leaving the page pending until timeout. */
        if (!pk_app_status_skip_section(app)) {
            status_finish_too_large(app);
        }
    }
    pk_app_log(app, PK_LOG_EVENT_QUERY, PK_LOG_ERR_NONE);
}

static bool probe_sections(pk_app_t *app, uint8_t skip_mask)
{
    pk_pending_t *p;
    int first;

    if (!app || !app->connected || !app->paired ||
        app->vehicle == PK_VEHICLE_NONE || app->busy || app->has_pending ||
        app->page != PK_PAGE_SETTINGS) {
        return false;
    }
    app->serial++;
    app->status_step = 0;
    app->status_skip_mask = skip_mask;
    app->status_need_send = false;
    app->field_count = 0;
    first = status_next_section(app, 0);
    if (first < 0) {
        app->status_skip_mask = 0;
        return false;
    }
    app->status_step = (uint8_t)first;
    app->status_silent = true;
    memset(&app->pending, 0, sizeof(app->pending));
    p = &app->pending;
    p->kind = PK_PENDING_STATUS;
    p->id = app->serial;
    p->scope = pk_app_scope(app);
    p->deadline = status_section_deadline(app);
    p->action = PK_ACTION_STATUS_READ;
    app->has_pending = true;
    pk_arbiter_invalidate(&app->keys);
    pk_app_set_busy(app, true);
    pk_ble_release_in_flight(app);
    if (pk_ble_send_status_step(app, p->deadline) == PK_ERR_BLOCKED) {
        app->has_pending = false;
        app->status_silent = false;
        app->status_skip_mask = 0;
        pk_app_set_busy(app, false);
        return false;
    }
    return true;
}

bool pk_app_probe_caps(pk_app_t *app)
{
    /* Only charge (0) and closures (3): the sections that carry port and
     * sentry capability. Climate/drive/tires are skipped. */
    return probe_sections(app, (uint8_t)((1u << 1) | (1u << 2) | (1u << 4)));
}

void pk_app_exit_status(pk_app_t *app)
{
    if (!app) {
        return;
    }
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    app->status_need_send = false;
    app->field_count = 0;
    app->lock_fsm = PK_LOCK_FSM_IDLE;
    pk_arbiter_invalidate(&app->keys);
    app->page = (app->origin == PK_ORIGIN_DRIVING || app->status_return_page != PK_PAGE_CARS)
                  ? PK_PAGE_MAIN : PK_PAGE_CARS;
    app->origin = PK_ORIGIN_LIST;
    pk_app_set_msg(app, PK_MSG_STATUS_EXIT);
}

bool pk_app_status_field(pk_app_t *app, const pk_status_input_t *in)
{
    pk_status_field_t *f = NULL;
    if (app && app->status_silent) {
        return false;
    }
    pk_scope_t cur;
    const char *timeliness = "未知";
    unsigned i;

    if (!app || !app->has_pending || !in || !in->scope || !in->name) {
        return false;
    }
    cur = pk_app_scope(app);
    if (app->pending.kind != PK_PENDING_STATUS ||
        in->request_id != app->pending.id || !pk_scope_full_match(in->scope, &cur) ||
        !pk_scope_full_match(&app->pending.scope, &cur) ||
        pk_time_due(app->now, app->pending.deadline)) {
        return false;
    }
    for (i = 0; i < app->field_count; i++) {
        if (strcmp(app->fields[i].name, in->name) == 0) {
            f = &app->fields[i];
            break;
        }
    }
    if (!f) {
        if (app->field_count >= PK_FIELD_CAP) {
            return false;
        }
        f = &app->fields[app->field_count++];
    }
    if (in->sample_present && in->clock_verified) {
        /* Production has no synthetic WALL epoch. Age mapping is 待真机. */
        timeliness = "本次读取窗口";
    }
    memset(f, 0, sizeof(*f));
    strncpy(f->name, in->name, sizeof(f->name) - 1);
    f->present = in->present;
    f->unit_verified = in->unit_verified;
    f->clock_verified = in->clock_verified;
    f->sample_present = in->sample_present;
    f->sampled = in->sampled;
    f->read_at = app->now;
    strncpy(f->timeliness, timeliness, sizeof(f->timeliness) - 1);
    /* Range without a verified unit, and power always, stay unshown. */
    if (in->present && (strcmp(in->name, "range") != 0 || in->unit_verified) &&
        strcmp(in->name, "power") != 0) {
        f->has_value = true;
        f->value = in->value;
    } else {
        f->has_value = false;
    }
    strncpy(app->status_phase, "partial", sizeof(app->status_phase) - 1);
    return true;
}

bool pk_app_status_advance(pk_app_t *app)
{
    int next;

    if (!app || !app->has_pending || app->pending.kind != PK_PENDING_STATUS) {
        return false;
    }
    next = status_next_section(app, (int)app->status_step + 1);
    if (next >= 0) {
        app->status_step = (uint8_t)next;
        app->pending.deadline = status_section_deadline(app);
        /* The reply is handled by the app task, but keep the next section in the
         * app tick so RX handling stays bounded and one write is never nested
         * inside another result handler. */
        app->status_need_send = true;
        return true;
    }
    app->status_need_send = false;
    if (app->status_silent) {
        app->status_silent = false;
        app->status_skip_mask = 0;
        app->status_phase[0] = '\0';
        pk_ble_release_in_flight(app);
        app->has_pending = false;
        pk_app_set_busy(app, false);
        return false;
    }
    strncpy(app->status_phase, "complete", sizeof(app->status_phase) - 1);
    pk_ble_release_in_flight(app);
    app->has_pending = false;
    pk_app_set_busy(app, false);
    return false;
}

bool pk_app_status_skip_section(pk_app_t *app)
{
    int next;

    if (!app || !app->has_pending || app->pending.kind != PK_PENDING_STATUS) {
        return false;
    }
    if (app->status_step < (uint8_t)PK_STATUS_SECTION_COUNT) {
        app->status_skip_mask |= (uint8_t)(1u << app->status_step);
    }
    next = status_next_section(app, (int)app->status_step + 1);
    if (next < 0) {
        return false;
    }
    pk_ble_release_in_flight(app);
    app->status_step = (uint8_t)next;
    app->pending.deadline = status_section_deadline(app);
    app->status_need_send = true;
    strncpy(app->status_phase, "partial", sizeof(app->status_phase) - 1);
    return true;
}

void pk_app_status_pump(pk_app_t *app)
{
    if (!app || !app->status_need_send) {
        return;
    }
    app->status_need_send = false;
    if (!app->has_pending || app->pending.kind != PK_PENDING_STATUS ||
        pk_time_due(app->now, app->pending.deadline)) {
        return;
    }
    pk_ble_release_in_flight(app);
    if (pk_ble_send_status_step(app, app->pending.deadline) == PK_ERR_BLOCKED) {
        /* The negotiated MTU shrank after section selection. Skip it and
         * continue; never leave the page waiting for a write that cannot fit. */
        if (!pk_app_status_skip_section(app)) {
            status_finish_too_large(app);
        }
    }
}
