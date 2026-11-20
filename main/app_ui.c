#include "app_ui.h"
#include "app_ui_internal.h"
#include "app_icons.h"
#include "app_ports.h"
#include "pk_copy.h"
#include "pk_risk.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_top_bar;
static lv_obj_t *s_bar_ico;
static lv_obj_t *s_bar_left_lbl;
static lv_obj_t *s_bar_mid_lbl;
static lv_obj_t *s_bar_right_lbl;
static lv_obj_t *s_bar_batt_body;
static lv_obj_t *s_bar_batt_cap;
static lv_obj_t *s_bar_batt_fill;

static lv_obj_t *s_foot;
static lv_obj_t *s_foot_up;
static lv_obj_t *s_foot_dot;
static lv_obj_t *s_foot_down;
static lv_obj_t *s_foot_sub;

static app_ui_ctx_t s_ctx;
static uint64_t s_last_sig_hash;
static bool s_has_sig;

static uint64_t fnv1a_u32(uint64_t h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        h ^= (uint8_t)(v & 0xFFu);
        h *= 1099511628211ULL;
        v >>= 8;
    }
    return h;
}

static uint64_t fnv1a_str(uint64_t h, const char *s)
{
    if (s) {
        while (*s) {
            h ^= (uint8_t)*s++;
            h *= 1099511628211ULL;
        }
    }
    h ^= 0xFFu;
    h *= 1099511628211ULL;
    return h;
}

static int ble_rssi_tier(const pk_app_t *app)
{
    int8_t rssi = 0;
    if (!app || !app->connected || !app_ports_ble_rssi(&rssi)) {
        return 0;
    }
    if (rssi >= -65) {
        return 3;
    }
    if (rssi >= -82) {
        return 2;
    }
    return 1;
}

static bool is_hold_gauge_active(const pk_app_t *app, pk_action_t *act_out,
                                 pk_ms_t *elapsed_out, pk_ms_t *total_out,
                                 const char **cancel_hint_out)
{
    if (app->page == PK_PAGE_MAIN && app->side.active && !app->side.consumed) {
        *act_out = (app->side.key == PK_KEY_UP) ? PK_ACTION_FRUNK_OPEN : PK_ACTION_TRUNK_MOVE;
        *total_out = pk_action_hold_ms(*act_out);
        *elapsed_out = (app->now >= app->side.at) ? (app->now - app->side.at) : 0;
        if (*elapsed_out > *total_out) {
            *elapsed_out = *total_out;
        }
        *cancel_hint_out = (app->side.key == PK_KEY_UP) ? "松开▲取消" : "松开▼取消";
        return true;
    }
    return false;
}

static void update_top_bar(const pk_app_t *app)
{
    char rbuf[32];
    app_icon_id_t l_ico = app->connected ? APP_ICON_BT_CONNECT : APP_ICON_BT_OFF;
    uint32_t l_col = app->connected ? COL_WHITE : COL_MUTED;
    static const char *const k_bt_lbls[4] = {
        "已连接", "已连接·弱", "已连接·中", "已连接·强"
    };
    const char *l_txt = app->connected
                            ? k_bt_lbls[ble_rssi_tier(app)]
                            : (app->vehicle == PK_VEHICLE_NONE ? "未添加车辆" : "未连接");

    if (app->page == PK_PAGE_CARS) {
        l_ico = APP_ICON_TESLA_CAR;
        l_col = COL_MUTED;
        l_txt = "车辆列表";
    } else if (app->page == PK_PAGE_SETTINGS && !app->edit_open) {
        l_ico = app->settings_quick_mode ? APP_ICON_TESLA_FLASH : APP_ICON_COG;
        l_col = COL_MUTED;
        l_txt = app->settings_quick_mode ? "快捷车控" : "设置";
    } else if (app->page == PK_PAGE_STATUS) {
        l_ico = APP_ICON_CLIPBOARD;
        l_col = COL_MUTED;
        l_txt = "车辆状态";
    }

    update_icon(s_bar_ico, l_ico, l_col);
    lv_label_set_text(s_bar_left_lbl, l_txt);
    lv_label_set_text(s_bar_mid_lbl, "");

    if (app->page == PK_PAGE_CARS) {
        snprintf(rbuf, sizeof(rbuf), "%d / 3", pk_store_count(&app->store));
    } else if (app->page == PK_PAGE_PAIR) {
        snprintf(rbuf, sizeof(rbuf), "配对 %d/4", app->pair_step ? app->pair_step : 1);
    } else if (app->battery_known) {
        snprintf(rbuf, sizeof(rbuf), "本机 %d%%", app->battery_percent);
    } else {
        /* G-06: unknown battery is "—", never "0%" */
        snprintf(rbuf, sizeof(rbuf), "本机 —");
    }
    lv_label_set_text(s_bar_right_lbl, rbuf);
    {
        uint32_t frame_col = app->low_battery ? COL_ALERT
                           : (app->battery_known ? COL_MUTED : COL_TAG_BORDER);
        lv_obj_set_style_border_color(s_bar_batt_body, lv_color_hex(frame_col), 0);
        lv_obj_set_style_bg_color(s_bar_batt_cap, lv_color_hex(frame_col), 0);

        if (!app->battery_known || app->battery_percent <= 0) {
            lv_obj_add_flag(s_bar_batt_fill, LV_OBJ_FLAG_HIDDEN);
        } else {
            int pct = app->battery_percent > 100 ? 100 : app->battery_percent;
            int fill_w = 1 + (pct * 12 + 50) / 100;
            uint32_t fill_col = (app->low_battery || pct <= 10) ? COL_ALERT
                              : (pct <= 20) ? 0xF59E0B
                              : (pct >= 80) ? 0x22C55E
                              : COL_WHITE;
            if (fill_w > 13) {
                fill_w = 13;
            }
            lv_obj_clear_flag(s_bar_batt_fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_bar_batt_fill, fill_w);
            lv_obj_set_style_bg_color(s_bar_batt_fill, lv_color_hex(fill_col), 0);
        }
    }
}

typedef struct {
    const char *up;
    const char *dot;
    const char *down;
    const char *sub;
    uint32_t    dot_col;
} footer_spec_t;

static footer_spec_t footer_for_hold(pk_action_t hold_act)
{
    if (hold_act == PK_ACTION_FRUNK_OPEN) {
        return (footer_spec_t){ "▲ 按住", "● —", "▼ —", "松开▲取消", COL_MUTED };
    }
    if (hold_act == PK_ACTION_TRUNK_MOVE) {
        return (footer_spec_t){ "▲ —", "● —", "▼ 按住", "松开▼取消", COL_MUTED };
    }
    return (footer_spec_t){ "▲ —", "● 按住", "▼ 取消", "松开●立即取消", COL_WHITE };
}

static footer_spec_t footer_for_main(const pk_app_t *app)
{
    if (app->vehicle == PK_VEHICLE_NONE) {
        return (footer_spec_t){ "▲ 不可用", "● 不可用", "▼ 不可用", "双击● 打开车辆列表", COL_MUTED };
    }
    if (!app->connected) {
        return (footer_spec_t){ "▲ 不可用", "● 重连", "▼ 不可用", "双击● 打开车辆列表", COL_WHITE };
    }
    if (app->moving) {
        return (footer_spec_t){ "▲ 不可用", "● 闭锁", "▼ 不可用", "长按●设置 · 双击状态", COL_WHITE };
    }
    if (!pk_app_parked(app)) {
        return (footer_spec_t){ "▲ 不可用", "● 闭锁", "▼ 不可用", "长按●设置 · 双击状态", COL_WHITE };
    }
    if (pk_app_primary(app) == PK_ACTION_UNLOCK) {
        return (footer_spec_t){ "▲ 前备箱", "● 开锁", "▼ 后备箱", "长按●设置 · 双击状态", COL_ALERT };
    }
    return (footer_spec_t){ "▲ 前备箱", "● 闭锁", "▼ 后备箱", "长按●设置 · 双击状态", COL_WHITE };
}

static footer_spec_t footer_for_page(const pk_app_t *app)
{
    static const struct {
        pk_page_t     page;
        footer_spec_t spec;
    } k_static_footers[] = {
        { PK_PAGE_VIN,     { "▲ —",        "● 等待提交", "▼ 取消",     "手机提交17位VIN（禁I O Q）", COL_WHITE } },
        { PK_PAGE_WAIT,    { "▲ —",        "● 退出等待", "▼ —",        "退出只停止本地等待",         COL_WHITE } },
        { PK_PAGE_RESULT,  { "▲ —",        "● 返回",     "▼ —",        "再发送需新操作，不自动重发", COL_WHITE } },
        { PK_PAGE_OFFLINE, { "▲ 闪灯禁用", "● 重连",     "▼ 鸣笛禁用", "鸣笛须先重连",        COL_WHITE } },
        { PK_PAGE_LOGS,    { "▲ 上条",     "● 返回设置", "▼ 下条",     "双击● 返回",             COL_WHITE } },
    };
    size_t i;

    for (i = 0; i < sizeof(k_static_footers) / sizeof(k_static_footers[0]); i++) {
        if (k_static_footers[i].page == app->page) {
            return k_static_footers[i].spec;
        }
    }

    switch (app->page) {
    case PK_PAGE_MAIN:
        return footer_for_main(app);
    case PK_PAGE_CARS: {
        const char *down = "▼ 下循环";
        if (app->car_focus >= 0 && pk_store_slot_used(&app->store, app->car_focus)) {
            bool can_del = pk_app_management_allowed(app, PK_MGMT_DELETE,
                                                     app->car_focus == app->vehicle, NULL);
            down = can_del ? "▼ 删除" : "▼ 受保护";
        }
        return (footer_spec_t){ "▲ 上循环", "● 选定", down, "双击● 返回", COL_WHITE };
    }
    case PK_PAGE_SETTINGS:
        return app->edit_open
            ? (footer_spec_t){ "▲ 上／减", "● 进入／确认", "▼ 下／加", "编辑值后回到保存或取消", COL_WHITE }
            : (footer_spec_t){ "▲ 上循环", "● 执行/进入", "▼ 下循环", "双击● 返回",         COL_WHITE };
    case PK_PAGE_STATUS:
        return (footer_spec_t){
            "▲ —",
            app->connected ? "● 重读状态" : ((app->origin == PK_ORIGIN_DRIVING) ? "● 返回主屏" : "● 返回列表"),
            "▼ —", "双击● 返回", COL_WHITE
        };
    case PK_PAGE_PAIR:
        return (footer_spec_t){
            "▲ 上一步",
            (app->pair_step == 4) ? "● 完成配对" : "● 下一步",
            "▼ 取消", "第 4 步才可完成配对", COL_WHITE
        };
    default:
        return (footer_spec_t){ "▲ 切换", "● 确定", "▼ 切换", "双击● 返回", COL_WHITE };
    }
}

static void update_footer(const pk_app_t *app, bool hold_active, pk_action_t hold_act)
{
    footer_spec_t f = hold_active ? footer_for_hold(hold_act) : footer_for_page(app);

    lv_label_set_text(s_foot_up, f.up);
    lv_label_set_text(s_foot_dot, f.dot);
    lv_obj_set_style_text_color(s_foot_dot, lv_color_hex(f.dot_col), 0);
    lv_label_set_text(s_foot_down, f.down);
    lv_label_set_text(s_foot_sub, f.sub);
}

void app_ui_init(void)
{
    s_scr = lv_obj_create(NULL);
    style_reset_box(s_scr, COL_BG, 0, 0, 0);

    /* 1. Top Bar (240x22) */
    s_top_bar = ui_create_box(s_scr, 0, 0, 240, 22, COL_BAR_BG, 0, 0);
    lv_obj_set_style_border_side(s_top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_top_bar, 1, 0);
    lv_obj_set_style_border_color(s_top_bar, lv_color_hex(COL_BAR_BORDER), 0);

    s_bar_ico = create_icon(s_top_bar, APP_ICON_BT_OFF, COL_MUTED, 20, 2);
    s_bar_left_lbl = create_lbl(s_top_bar, font_12(), COL_MUTED, 42, 3, 76, "未添加车辆");
    s_bar_mid_lbl = create_lbl(s_top_bar, font_12(), COL_MUTED, 116, 3, 12, "");
    s_bar_right_lbl = create_lbl(s_top_bar, font_12(), COL_MUTED, 120, 3, 76, "本机 —");
    lv_obj_set_style_text_align(s_bar_right_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_bar_batt_body = ui_create_box(s_top_bar, 200, 6, 17, 10, COL_BAR_BG, COL_MUTED, 2);
    s_bar_batt_cap = ui_create_box(s_top_bar, 217, 9, 2, 4, COL_MUTED, 0, 1);
    s_bar_batt_fill = ui_create_box(s_bar_batt_body, 1, 1, 0, 6, COL_MUTED, 0, 1);
    lv_obj_add_flag(s_bar_batt_fill, LV_OBJ_FLAG_HIDDEN);

    /* 2. Body Container (240x262 at y=22) */
    s_ctx.body = ui_create_box(s_scr, 0, 22, 240, 262, COL_BG, 0, 0);

    /* 3. Bottom Footer (240x36 at y=284) */
    s_foot = ui_create_box(s_scr, 0, 284, 240, 36, COL_FOOT_BG, 0, 0);
    lv_obj_set_style_border_side(s_foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_foot, 1, 0);
    lv_obj_set_style_border_color(s_foot, lv_color_hex(COL_FOOT_BORDER), 0);

    s_foot_up = create_lbl(s_foot, font_12(), COL_MUTED, 8, 2, 74, "▲ 前备箱");
    s_foot_dot = create_lbl(s_foot, font_12(), COL_WHITE, 78, 2, 84, "● 闭锁");
    lv_obj_set_style_text_align(s_foot_dot, LV_TEXT_ALIGN_CENTER, 0);
    s_foot_down = create_lbl(s_foot, font_12(), COL_MUTED, 158, 2, 74, "▼ 后备箱");
    lv_obj_set_style_text_align(s_foot_down, LV_TEXT_ALIGN_RIGHT, 0);

    s_foot_sub = create_lbl(s_foot, font_12(), COL_MUTED, 8, 18, 224, "长按●设置 · 双击状态");
    lv_obj_set_style_text_align(s_foot_sub, LV_TEXT_ALIGN_CENTER, 0);

    s_has_sig = false;
    lv_screen_load(s_scr);
}

static void ui_clean_body(void)
{
    s_ctx.hold_arc = NULL;
    s_ctx.hold_time_lbl = NULL;
    s_ctx.hold_rendered_act = PK_ACTION_NONE;
    s_ctx.settings_row_count = 0;
    s_ctx.settings_rendered_start = -1;
    memset(s_ctx.settings_rows, 0, sizeof(s_ctx.settings_rows));
    lv_obj_clean(s_ctx.body);
}

static uint64_t compute_ui_sig(const pk_app_t *app, bool hold_active, pk_ms_t hold_elapsed)
{
    uint64_t h = 14695981039346656037ULL;
    unsigned i;

    h = fnv1a_u32(h, (uint32_t)app->page);
    h = fnv1a_u32(h, app->awake ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->vehicle);
    h = fnv1a_u32(h, app->connected ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)ble_rssi_tier(app));
    h = fnv1a_u32(h, pk_app_parked(app) ? 1u : 0u);
    h = fnv1a_u32(h, app->moving ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)pk_app_primary(app));
    h = fnv1a_u32(h, (uint32_t)(app->battery_known ? app->battery_percent : -1));
    h = fnv1a_u32(h, app->low_battery ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->car_focus);
    h = fnv1a_u32(h, (uint32_t)app->focus);
    h = fnv1a_u32(h, (uint32_t)app->offer_focus);
    h = fnv1a_u32(h, (uint32_t)app->delete_focus);
    h = fnv1a_u32(h, (uint32_t)app->reset_focus);
    h = fnv1a_u32(h, (uint32_t)app->log_focus);
    h = fnv1a_u32(h, (uint32_t)app->logs.count);
    h = fnv1a_u32(h, app->edit_open ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->edit_key);
    h = fnv1a_u32(h, (uint32_t)app->editor_index);
    h = fnv1a_u32(h, app->editing ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->brightness);
    h = fnv1a_u32(h, (uint32_t)app->power);
    h = fnv1a_u32(h, app->settings_quick_mode ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->draft.brightness);
    h = fnv1a_u32(h, (uint32_t)app->draft.power);
    h = fnv1a_u32(h, (uint32_t)app->draft.bool_enabled);
    h = fnv1a_u32(h, (uint32_t)app->draft.seat_position);
    h = fnv1a_u32(h, (uint32_t)app->draft.seat_level);
    h = fnv1a_u32(h, app->ap ? 1u : 0u);
    h = fnv1a_u32(h, app->has_vin ? 1u : 0u);
    h = fnv1a_str(h, app->vin);
    h = fnv1a_u32(h, app->reconnect.active ? 1u : 0u);
    h = fnv1a_u32(h, (uint32_t)app->pair_step);
    if (app->page == PK_PAGE_PAIR) {
        h = fnv1a_u32(h, (uint32_t)app_ports_ble_pair_state(app));
    }
    h = fnv1a_u32(h, (uint32_t)app->auth);
    h = fnv1a_u32(h, (uint32_t)app->lock_fsm);
    h = fnv1a_u32(h, app->store.live.generation);
    h = fnv1a_u32(h, (uint32_t)(hold_active ? (hold_elapsed / 100u) : 0u));
    h = fnv1a_str(h, app->status_phase);
    h = fnv1a_u32(h, (uint32_t)app->field_count);
    for (i = 0; i < app->field_count; i++) {
        h = fnv1a_str(h, app->fields[i].name);
        h = fnv1a_u32(h, app->fields[i].has_value ? 1u : 0u);
        h = fnv1a_u32(h, (uint32_t)app->fields[i].value);
        h = fnv1a_u32(h, app->fields[i].unit_verified ? 1u : 0u);
        h = fnv1a_u32(h, app->fields[i].sample_present ? 1u : 0u);
    }
    h = fnv1a_str(h, pk_copy(app->message));
    h = fnv1a_str(h, app->message_extra);
    return h;
}

bool app_ui_needs_update(const pk_app_t *app)
{
    pk_action_t hold_act = PK_ACTION_NONE;
    pk_ms_t hold_elapsed = 0;
    pk_ms_t hold_total = 0;
    const char *hold_hint = NULL;
    bool hold_active;
    uint64_t h;

    if (!app || !s_scr) {
        return false;
    }
    if (!s_has_sig) {
        return true;
    }
    hold_active = is_hold_gauge_active(app, &hold_act, &hold_elapsed, &hold_total, &hold_hint);
    h = compute_ui_sig(app, hold_active, hold_elapsed);
    return h != s_last_sig_hash;
}

static bool try_update_hold_gauge_incremental(pk_action_t hold_act,
                                              pk_ms_t hold_elapsed, pk_ms_t hold_total)
{
    char tbuf[32];
    int pct;

    if (!s_ctx.hold_arc || !s_ctx.hold_time_lbl || s_ctx.hold_rendered_act != hold_act) {
        return false;
    }
    pct = hold_total ? (int)((hold_elapsed * 100u) / hold_total) : 0;
    lv_arc_set_value(s_ctx.hold_arc, pct);
    snprintf(tbuf, sizeof(tbuf), "%u.%u 秒 / %u.%u 秒",
             (unsigned)(hold_elapsed / 1000u), (unsigned)((hold_elapsed % 1000u) / 100u),
             (unsigned)(hold_total / 1000u), (unsigned)((hold_total % 1000u) / 100u));
    lv_label_set_text(s_ctx.hold_time_lbl, tbuf);
    return true;
}

static bool try_update_settings_focus_incremental(const pk_app_t *app)
{
    int want_start;
    int r;

    if (app->page != PK_PAGE_SETTINGS || app->edit_open || s_ctx.settings_row_count <= 0 ||
        s_ctx.settings_rendered_quick != app->settings_quick_mode ||
        s_ctx.settings_rendered_msg != app->message) {
        return false;
    }
    want_start = app->settings_quick_mode ? 0 : app_ui_settings_window_start((int)app->focus);
    if (want_start != s_ctx.settings_rendered_start ||
        app->focus == PK_SETTING_POWER || app->focus == PK_SETTING_BRIGHTNESS) {
        return false;
    }
    for (r = 0; r < s_ctx.settings_row_count; r++) {
        bool on = (s_ctx.settings_row_ids[r] == app->focus);
        lv_obj_set_style_bg_color(s_ctx.settings_rows[r], lv_color_hex(on ? COL_ON_BG : COL_BG), 0);
        lv_obj_set_style_border_color(s_ctx.settings_rows[r],
                                      lv_color_hex(on ? COL_ON_BORDER : COL_BG), 0);
        lv_obj_set_style_border_width(s_ctx.settings_rows[r], on ? 1 : 0, 0);
    }
    return true;
}

void app_ui_update(const pk_app_t *app)
{
    uint64_t h;
    pk_action_t hold_act = PK_ACTION_NONE;
    pk_ms_t hold_elapsed = 0;
    pk_ms_t hold_total = 0;
    const char *hold_hint = NULL;
    bool hold_active;

    if (!app || !s_scr) {
        return;
    }

    hold_active = is_hold_gauge_active(app, &hold_act, &hold_elapsed, &hold_total, &hold_hint);
    h = compute_ui_sig(app, hold_active, hold_elapsed);
    if (s_has_sig && h == s_last_sig_hash) {
        return;
    }
    s_last_sig_hash = h;
    s_has_sig = true;

    if (hold_active) {
        if (try_update_hold_gauge_incremental(hold_act, hold_elapsed, hold_total)) {
            return;
        }
        update_top_bar(app);
        update_footer(app, hold_active, hold_act);
        ui_clean_body();
        app_ui_render_hold_gauge(&s_ctx, hold_act, hold_elapsed, hold_total, hold_hint);
        return;
    }

    update_top_bar(app);
    update_footer(app, hold_active, hold_act);

    s_ctx.hold_arc = NULL;
    s_ctx.hold_time_lbl = NULL;
    s_ctx.hold_rendered_act = PK_ACTION_NONE;

    if (try_update_settings_focus_incremental(app)) {
        return;
    }

    ui_clean_body();

    switch (app->page) {
    case PK_PAGE_MAIN:
        app_ui_render_page_main(s_ctx.body, app);
        break;
    case PK_PAGE_CARS:
        app_ui_render_page_cars(s_ctx.body, app);
        break;
    case PK_PAGE_SETTINGS:
        app_ui_render_page_settings(&s_ctx, app);
        break;
    case PK_PAGE_STATUS:
        app_ui_render_page_status(s_ctx.body, app);
        break;
    case PK_PAGE_VIN:
        app_ui_render_page_vin(s_ctx.body, app);
        break;
    case PK_PAGE_PAIR:
        app_ui_render_page_pair(s_ctx.body, app);
        break;
    case PK_PAGE_OFFLINE:
        app_ui_render_page_offline(s_ctx.body, app);
        break;
    case PK_PAGE_WAIT:
    case PK_PAGE_RESULT:
        app_ui_render_page_wait_or_result(s_ctx.body, app);
        break;
    case PK_PAGE_QUALIFICATION_OFFER:
    case PK_PAGE_DELETE_CONFIRM:
    case PK_PAGE_RESET_CONFIRM:
        app_ui_render_page_dialog(s_ctx.body, app);
        break;
    case PK_PAGE_LOGS:
        app_ui_render_page_logs(s_ctx.body, app);
        break;
    default:
        app_ui_render_page_main(s_ctx.body, app);
        break;
    }
}
