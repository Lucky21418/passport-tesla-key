#include "app_ui_internal.h"
#include "app_icons.h"
#include "app_ports.h"
#include "pk_copy.h"
#include "pk_feedback.h"
#include "pk_risk.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * Settings Metadata Helpers (18 Rows, Table-Driven)
 * ========================================================================== */

typedef struct {
    const char *title;
    app_icon_id_t icon;
} pk_setting_meta_t;

static const pk_setting_meta_t k_setting_meta[PK_SETTING_COUNT] = {
    [PK_SETTING_BRIGHTNESS]   = { "屏幕亮度",     APP_ICON_BRIGHTNESS },
    [PK_SETTING_POWER]        = { "省电策略",     APP_ICON_MOON },
    [PK_SETTING_FLASH]        = { "寻车闪灯",     APP_ICON_TESLA_FLASH },
    [PK_SETTING_HORN]         = { "寻车鸣笛",     APP_ICON_TESLA_HORN },
    [PK_SETTING_VENT]         = { "车窗通风",     APP_ICON_TESLA_VENT },
    [PK_SETTING_CLIMATE]      = { "空调",         APP_ICON_TESLA_FAN },
    [PK_SETTING_SENTRY]       = { "哨兵",         APP_ICON_TESLA_SENTRY },
    [PK_SETTING_DEFOG]        = { "除雾",         APP_ICON_TESLA_FAN },
    [PK_SETTING_PORT]         = { "充电口",       APP_ICON_TESLA_PORT },
    [PK_SETTING_SEAT]         = { "座椅加热",     APP_ICON_SEAT_HEATER },
    [PK_SETTING_STEERING]     = { "方向盘加热",   APP_ICON_STEERING },
    [PK_SETTING_LOGS]         = { "日志与诊断",   APP_ICON_CLIPBOARD },
    [PK_SETTING_HOTSPOT]      = { "热点",         APP_ICON_WIFI },
    [PK_SETTING_RESET]        = { "恢复出厂",     APP_ICON_TRASH },
};

static const char *setting_title(pk_setting_id_t id)
{
    if ((unsigned)id < (unsigned)PK_SETTING_COUNT && k_setting_meta[id].title) {
        return k_setting_meta[id].title;
    }
    return "设置";
}

static app_icon_id_t setting_icon(pk_setting_id_t id)
{
    if ((unsigned)id < (unsigned)PK_SETTING_COUNT) {
        return k_setting_meta[id].icon;
    }
    return APP_ICON_COG;
}

static const char *setting_vehicle_prereq(const pk_app_t *app, pk_action_t action)
{
    if (app->vehicle == PK_VEHICLE_NONE) {
        return "未添加车辆";
    }
    if (!app->connected) {
        return "请先重连";
    }
    /* Same table as the command guard. An unknown park state is not a dead
     * end: pressing the row performs one check-before-act drive-state read
     * and then sends the frozen gesture. Only a proven moving state blocks. */
    if (app->moving && pk_action_moving_blocked(action)) {
        return "行驶中不可用";
    }
    return NULL;
}

static void format_parked_setting_val(const pk_app_t *app, pk_setting_id_t id, char *out, size_t n)
{
    static const struct {
        pk_setting_id_t id;
        const char     *text;
    } k_labels[] = {
        { PK_SETTING_SENTRY,       "开／关" },
        { PK_SETTING_PORT,         "长按开/关" },
        { PK_SETTING_VENT,         "长按通风" },
        { PK_SETTING_SEAT,         "座位·档位" },
        { PK_SETTING_STEERING,     "开／关" },
        { PK_SETTING_FLASH,        "请求闪灯" },
        { PK_SETTING_HORN,         "请求鸣笛" },
    };
    size_t i;

    for (i = 0; i < sizeof(k_labels) / sizeof(k_labels[0]); i++) {
        if (k_labels[i].id == id) {
            snprintf(out, n, "%s", k_labels[i].text);
            return;
        }
    }
    snprintf(out, n, "进入");
}

static void format_setting_val(const pk_app_t *app, pk_setting_id_t id, char *out, size_t n)
{
    const char *blocked;

    switch (id) {
    case PK_SETTING_POWER:
        snprintf(out, n, app->power == PK_POWER_ENDURANCE ? "续航优先" : "体验优先");
        return;
    case PK_SETTING_BRIGHTNESS:
        snprintf(out, n, "%d%%", app->brightness);
        return;
    case PK_SETTING_LOGS:
        snprintf(out, n, "仅脱敏记录");
        return;
    case PK_SETTING_HOTSPOT:
        snprintf(out, n, app->ap ? "录入中开启" : "已关闭·只读");
        return;
    case PK_SETTING_RESET:
        if (!pk_app_management_allowed(app, PK_MGMT_RESET, true, NULL)) {
            snprintf(out, n, "当前不可恢复");
        } else {
            snprintf(out, n, "二次确认");
        }
        return;
    case PK_SETTING_CLIMATE:
        blocked = setting_vehicle_prereq(app, PK_ACTION_CLIMATE_ON);
        if (blocked) {
            snprintf(out, n, "%s", blocked);
            return;
        }
        snprintf(out, n, "%s",
                 app->caps.climate_on == PK_CAP_YES ? "已开 · 开／关" :
                 (app->caps.climate_on == PK_CAP_NO ? "已关 · 开／关" : "开／关"));
        return;
    case PK_SETTING_DEFOG:
        blocked = setting_vehicle_prereq(app, PK_ACTION_PRECONDITION_MAX_ON);
        if (blocked) {
            snprintf(out, n, "%s", blocked);
            return;
        }
        snprintf(out, n, "%s",
                 app->caps.preconditioning == PK_CAP_YES ? "已开 · 开／关" :
                 (app->caps.preconditioning == PK_CAP_NO ? "已关 · 开／关" : "开／关"));
        return;
    default:
        blocked = setting_vehicle_prereq(app, pk_setting_action(id));
        if (blocked) {
            snprintf(out, n, "%s", blocked);
            return;
        }
        format_parked_setting_val(app, id, out, n);
        return;
    }
}

/* ==========================================================================
 * Page Renderers
 * ========================================================================== */

void app_ui_render_hold_gauge(app_ui_ctx_t *ctx, pk_action_t act,
                              pk_ms_t elapsed, pk_ms_t total, const char *cancel_hint)
{
    lv_obj_t *arc;
    app_icon_id_t ico = APP_ICON_TESLA_HOOD;
    const char *title = "继续按住，确认开启";
    char tbuf[32];
    int pct = total ? (int)((elapsed * 100u) / total) : 0;

    switch (act) {
    case PK_ACTION_FRUNK_OPEN:        ico = APP_ICON_TESLA_HOOD;  title = "继续按住，确认开前备箱"; break;
    case PK_ACTION_TRUNK_MOVE:        ico = APP_ICON_TESLA_TRUNK; title = "继续按住，确认动后备箱"; break;
    case PK_ACTION_CHARGE_PORT_OPEN:  ico = APP_ICON_TESLA_PORT;  title = "继续按住，开启充电口"; break;
    case PK_ACTION_CHARGE_PORT_CLOSE: ico = APP_ICON_TESLA_PORT;  title = "继续按住，关闭充电口"; break;
    case PK_ACTION_HONK:              ico = APP_ICON_TESLA_HORN;  title = "继续按住，确认鸣笛"; break;
    case PK_ACTION_CLIMATE_ON:        ico = APP_ICON_TESLA_FAN;   title = "继续按住，开启空调"; break;
    default: break;
    }

    arc = lv_arc_create(ctx->body);
    lv_obj_set_size(arc, 104, 104);
    lv_obj_set_pos(arc, (240 - 104) / 2, 24);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, pct);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_ARC_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_ARC_IND), LV_PART_INDICATOR);

    create_icon(ctx->body, ico, COL_WHITE, (240 - 20) / 2, 24 + (104 - 20) / 2);

    snprintf(tbuf, sizeof(tbuf), "%u.%u 秒 / %u.%u 秒",
             (unsigned)(elapsed / 1000u), (unsigned)((elapsed % 1000u) / 100u),
             (unsigned)(total / 1000u), (unsigned)((total % 1000u) / 100u));
    {
        lv_obj_t *tl = create_lbl(ctx->body, font_16(), COL_WHITE, 12, 140, 216, tbuf);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
        ctx->hold_time_lbl = tl;
    }
    ctx->hold_arc = arc;
    ctx->hold_rendered_act = act;
    {
        lv_obj_t *pt = create_lbl(ctx->body, font_16(), COL_WHITE, 12, 168, 216, title);
        lv_obj_set_style_text_align(pt, LV_TEXT_ALIGN_CENTER, 0);
    }
    {
        lv_obj_t *gs = create_lbl(ctx->body, font_12(), COL_MUTED, 12, 196, 216, cancel_hint);
        lv_obj_set_style_text_align(gs, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void app_ui_render_page_main(lv_obj_t *body, const pk_app_t *app)
{
    int y = 4;
    int zh = app->low_battery ? 42 : 52;
    int gap = app->low_battery ? 4 : 5;
    pk_action_t prim = pk_app_primary(app);
    const char *msg = pk_copy(app->message);

    if (app->low_battery) {
        build_banner(body, y, APP_ICON_BATT_10, "本机电量低，请充电，仍可闭锁");
        y += 30;
    }

    if (app->vehicle == PK_VEHICLE_NONE) {
        build_glance_card(body, y, 66, APP_ICON_TESLA_CAR, false,
                          "未添加车辆",
                          (msg && msg[0]) ? msg : "双击●进车辆列表添加");
        y += 71;
        build_zone_card(body, y, zh, false, false, true, "前备箱", NULL, "未添加车辆");
        y += zh + gap;
        build_zone_card(body, y, zh, true, false, false, "进入车辆列表", NULL, "双击 ●");
        y += zh + gap;
        build_zone_card(body, y, zh, false, false, true, "后备箱", NULL, "未添加车辆");
        return;
    }

    if (!app->connected) {
        build_glance_card(body, y, 66, APP_ICON_BT_OFF, false,
                          "请先重连",
                          (msg && msg[0]) ? msg : "单击●重连，不排队补发");
        y += 71;
        build_zone_card(body, y, zh, false, false, true, "前备箱", NULL, "请先重连");
        y += zh + gap;
        build_zone_card(body, y, zh, true, false, false, "重新连接", NULL, "单击 ●");
        y += zh + gap;
        build_zone_card(body, y, zh, false, false, true, "后备箱", NULL, "请先重连");
        return;
    }

    if (app->moving) {
        /* Driving: main title, with trunk & defog note in subtitle. */
        build_glance_card(body, y, 66, APP_ICON_TESLA_CAR, false,
                          "行驶中",
                          pk_copy(PK_MSG_TRUNK_UNAVAILABLE));
        y += 71;
        build_zone_card(body, y, zh, false, false, true, "前备箱", NULL, "行驶中不可用");
        y += zh + gap;
        build_zone_card(body, y, zh, true, false, false, "闭锁", NULL, "单击 ●");
        y += zh + gap;
        build_zone_card(body, y, zh, false, false, true, "后备箱", NULL, "行驶中不可用");
        return;
    }

    {
        const char *side_hint = "长按 ▲";
        const char *trunk_hint = "长按 ▼";
        const char *gtitle;
        const char *gsub;
        app_icon_id_t gico;
        bool gwarn = false;

        if (prim == PK_ACTION_UNLOCK) {
            gico = APP_ICON_TESLA_LOCK;
            gtitle = "已闭锁";
            gsub = (msg && msg[0]) ? msg : "车门已锁 · 单击●开锁";
            build_glance_card(body, y, 66, gico, false, gtitle, gsub);
            y += 71;
            build_zone_card(body, y, zh, false, false, false, "前备箱", NULL, side_hint);
            y += zh + gap;
            build_zone_card(body, y, zh, true, true, false, "开锁", NULL, "单击 ●");
            y += zh + gap;
            build_zone_card(body, y, zh, false, false, false, "后备箱", NULL, trunk_hint);
        } else {
            bool known_unlocked = (app->vcsec_status_valid &&
                                   app->vcsec_lock_state == PK_LOCK_READ_UNLOCKED) ||
                                  (app->has_grant && app->grant.locked_known && !app->grant.locked);
            gico = known_unlocked ? APP_ICON_LOCK_OPEN : APP_ICON_TESLA_CAR;
            gtitle = known_unlocked ? "未闭锁" : "已连接";
            gwarn = known_unlocked;
            gsub = (msg && msg[0]) ? msg
                 : (known_unlocked ? "车辆未锁 · 单击●闭锁" : "单击●闭锁 · 双击●查状态");
            build_glance_card(body, y, 66, gico, gwarn, gtitle, gsub);
            y += 71;
            build_zone_card(body, y, zh, false, false, false, "前备箱", NULL, side_hint);
            y += zh + gap;
            build_zone_card(body, y, zh, true, false, false, "闭锁", NULL, "单击 ●");
            y += zh + gap;
            build_zone_card(body, y, zh, false, false, false, "后备箱", NULL, trunk_hint);
        }
    }
}

void app_ui_render_page_cars(lv_obj_t *body, const pk_app_t *app)
{
    int items[8];
    int n = pk_app_list_items(app, items, 8);
    int focus_idx = 0;
    int start = 0;
    int i;
    int y = 5;
    const char *msg = pk_copy(app->message);

    for (i = 0; i < n; i++) {
        if (items[i] == app->car_focus) {
            focus_idx = i;
            break;
        }
    }
    if (n > 4 && focus_idx >= 3) {
        start = focus_idx - 3;
    }

    if (msg && msg[0]) {
        create_lbl(body, font_12(), COL_MUTED, 8, y, 224, msg);
        y += 18;
    }

    for (i = start; i < n && i < start + 4; i++) {
        int item = items[i];
        bool on = (item == app->car_focus);
        if (item >= 0) {
            const pk_vehicle_rec_t *rec = pk_store_get(&app->store, item);
            bool is_cur = (item == app->vehicle);
            char nm[32];
            char tail[16];
            lv_obj_t *slot = ui_create_box(body, UI_MARGIN_X, y, UI_CONTENT_W, 48,
                                           on ? COL_ON_BG : COL_CARD_BG,
                                           on ? COL_ON_BORDER : COL_CARD_BORDER, 4);

            snprintf(nm, sizeof(nm), "绑定车辆 %d", item + 1);
            create_lbl(slot, font_12(), COL_WHITE, 8, 5, 148, nm);

            if (is_cur) {
                lv_obj_t *tag = ui_create_box(slot, 178, 4, 38, 19,
                                              COL_BADGE_BG, COL_TAG_BORDER, 3);
                create_lbl(tag, font_12(), COL_WHITE, 4, 0, 30, "当前");
            }

            create_icon(slot, (is_cur && app->connected) ? APP_ICON_BT_CONNECT : APP_ICON_BT_OFF,
                        (is_cur && app->connected) ? COL_WHITE : COL_MUTED, 8, 24);
            create_lbl(slot, font_12(), (is_cur && app->connected) ? COL_WHITE : COL_MUTED,
                       30, 26, 92,
                       (is_cur && app->connected) ? "已连接" : "未连接");
            app_ports_format_masked_vin(rec ? rec->vin : NULL, tail, sizeof(tail));
            {
                lv_obj_t *vl = create_lbl(slot, font_12(), COL_MUTED, 134, 26, 82, tail);
                lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
            }
            y += 53;
        } else {
            app_icon_id_t ico = APP_ICON_PLUS;
            const char *ttl = "添加车辆";
            const char *val = "录入VIN → 配对";
            lv_obj_t *row;

            if (item == PK_LIST_STATUS) {
                ico = APP_ICON_CLIPBOARD;
                ttl = "车辆状态";
                val = "进入";
            } else if (item == PK_LIST_SETTINGS) {
                ico = APP_ICON_COG;
                ttl = "设置";
                val = "进入";
            }
            row = ui_create_box(body, UI_MARGIN_X, y, UI_CONTENT_W, 34,
                                on ? COL_ON_BG : COL_CARD_BG,
                                on ? COL_ON_BORDER : COL_CARD_BORDER, 4);
            create_icon(row, ico, COL_MUTED, 8, 7);
            create_lbl(row, font_12(), COL_WHITE, 32, 9, 76, ttl);
            {
                lv_obj_t *rv = create_lbl(row, font_12(), COL_MUTED, 108, 9, 110, val);
                lv_obj_set_style_text_align(rv, LV_TEXT_ALIGN_RIGHT, 0);
            }
            y += 38;
        }
    }
}

static lv_obj_t *build_setting_row(lv_obj_t *parent, int y, int w,
                                   pk_setting_id_t id, bool on, const pk_app_t *app)
{
    bool danger = (id == PK_SETTING_RESET);
    char val[40];
    lv_obj_t *r = ui_create_box(parent, UI_MARGIN_X, y, w, 28,
                                on ? COL_ON_BG : COL_BG,
                                on ? COL_ON_BORDER : 0, 4);
    int val_x = (w >= UI_CONTENT_W) ? 130 : 126;

    create_icon(r, setting_icon(id), danger ? COL_ALERT : COL_MUTED, 5, 4);
    create_lbl(r, font_12(), danger ? COL_ALERT : COL_WHITE, 30, 6, 92, setting_title(id));

    format_setting_val(app, id, val, sizeof(val));
    {
        lv_obj_t *vl = create_lbl(r, font_12(), danger ? COL_ALERT : COL_MUTED, val_x, 6, 86, val);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
    }
    return r;
}

int app_ui_settings_window_start(int focus)
{
    (void)focus;
    return 0;
}

static void format_setting_editor_fields(const pk_app_t *app, char *info, size_t info_n,
                                         char *f0, size_t f0_n, char *f1, size_t f1_n)
{
    pk_setting_id_t k = app->edit_key;
    f0[0] = f1[0] = '\0';
    switch (k) {
    case PK_SETTING_BRIGHTNESS:
        snprintf(info, info_n, "▲▼ 实时调节亮度，按●保存");
        snprintf(f0, f0_n, "亮度 %d%%", app->draft.brightness);
        return;
    case PK_SETTING_POWER:
        snprintf(info, info_n, "省电策略不会切断受保护的驾驶连接");
        snprintf(f0, f0_n, "模式：%s",
                 app->draft.power == PK_POWER_ENDURANCE ? "续航优先" : "体验优先");
        return;
    case PK_SETTING_STEERING:
        snprintf(info, info_n, "显式选择开启或关闭后再保存");
        snprintf(f0, f0_n, "状态：%s",
                 app->draft.bool_enabled == PK_CAP_YES ? "开启" :
                 (app->draft.bool_enabled == PK_CAP_NO ? "关闭" : "待选择"));
        return;
    case PK_SETTING_SEAT: {
        const char *s_name = "待选择";
        if (app->draft.seat_position == 0) s_name = "主驾(左前)";
        else if (app->draft.seat_position == 1) s_name = "副驾(右前)";
        else if (app->draft.seat_position == 2) s_name = "左后";
        else if (app->draft.seat_position == 4) s_name = "右后";
        snprintf(info, info_n, "分别选择座位与档位，不群发全车");
        snprintf(f0, f0_n, "座位：%s", s_name);
        if (app->draft.seat_level >= 0) {
            snprintf(f1, f1_n, "加热：%d 档", app->draft.seat_level);
        } else {
            snprintf(f1, f1_n, "加热：待选择");
        }
        return;
    }
    default:
        snprintf(info, info_n, "请选择参数后保存");
        snprintf(f0, f0_n, "参数");
        return;
    }
}

static void render_setting_editor(lv_obj_t *body, const pk_app_t *app, const char *msg)
{
    pk_setting_id_t k = app->edit_key;
    char f0[48], f1[48], info[128];
    lv_obj_t *ic;
    int y = 102;

    build_glance_card(body, 6, 52, setting_icon(k), false,
                      setting_title(k), "设置同页编辑，非新菜单");
    format_setting_editor_fields(app, info, sizeof(info), f0, sizeof(f0), f1, sizeof(f1));
    if (msg && msg[0]) {
        snprintf(info, sizeof(info), "%s", msg);
    }

    ic = ui_create_box(body, UI_MARGIN_X, 62, UI_CONTENT_W, 36,
                       COL_CARD_BG, COL_CARD_BORDER, 4);
    (void)create_lbl_wrap(ic, font_12(), COL_MUTED, 8, 3, 208, info);

    if (k == PK_SETTING_SEAT) {
        build_zone_card(body, y, 32, app->editor_index == 0, false, false,
                        f0, NULL, (app->editor_index == 0 && app->editing) ? "▲▼ 调节" : "● 编辑");
        build_zone_card(body, y + 35, 32, app->editor_index == 1, false, false,
                        f1, NULL, (app->editor_index == 1 && app->editing) ? "▲▼ 调节" : "● 编辑");
        build_zone_card(body, y + 70, 32, app->editor_index == 2, false, false, "保存当前项", NULL, "●");
        build_zone_card(body, y + 105, 32, app->editor_index == 3, false, false, "取消草稿", NULL, "●");
        return;
    }

    {
        bool is_bright = (k == PK_SETTING_BRIGHTNESS);
        build_zone_card(body, y, 42, app->editor_index == 0, false, false, f0, NULL,
                        (app->editor_index == 0 && app->editing) ? "▲▼ 调节" : "● 编辑");
        build_zone_card(body, y + 46, 42, app->editor_index == 1, false, false,
                        "保存当前项", is_bright ? "保存亮度设置并返回" : "确认后执行或生效", "●");
        build_zone_card(body, y + 92, 42, app->editor_index == 2, false, false,
                        "取消草稿", is_bright ? "恢复原亮度并返回" : "不发送车辆命令", "●");
    }
}

void app_ui_render_page_settings(app_ui_ctx_t *ctx, const pk_app_t *app)
{
    int i;
    int focus = (int)app->focus;
    int y = 4;
    int b_cnt = 0;
    const pk_setting_id_t *k_basic = pk_app_quick_settings(&b_cnt);
    const char *msg = pk_copy(app->message);

    if (app->edit_open) {
        render_setting_editor(ctx->body, app, msg);
        return;
    }

    ctx->settings_row_count = 0;
    ctx->settings_rendered_msg = app->message;
    ctx->settings_rendered_start = 0;
    ctx->settings_rendered_quick = app->settings_quick_mode;

    create_lbl(ctx->body, font_12(), COL_MUTED, UI_MARGIN_X, y, UI_CONTENT_W,
               (msg && msg[0]) ? msg : "本机设备设置 · 按●执行或编辑");
    y += 20;
    for (i = 0; i < b_cnt && i < 7; i++) {
        pk_setting_id_t id = k_basic[i];
        ctx->settings_rows[i] = build_setting_row(ctx->body, y, UI_CONTENT_W, id,
                                                  (int)id == focus, app);
        ctx->settings_row_ids[i] = id;
        ctx->settings_row_count++;
        y += 32;
    }
}

static const pk_status_field_t *find_status_field(const pk_app_t *app, const char *name)
{
    unsigned i;
    for (i = 0; i < app->field_count; i++) {
        if (strcmp(app->fields[i].name, name) == 0) {
            return &app->fields[i];
        }
    }
    return NULL;
}

static void render_status_energy_card(lv_obj_t *body, const pk_app_t *app)
{
    const pk_status_field_t *f_soc = find_status_field(app, "soc");
    const pk_status_field_t *f_range = find_status_field(app, "range");
    const pk_status_field_t *f_eta_full = find_status_field(app, "eta_full");
    const pk_status_field_t *f_temp = find_status_field(app, "cabin_temp");
    int soc_pct = (f_soc && f_soc->has_value) ? f_soc->value : -1;
    lv_obj_t *ec = ui_create_box(body, UI_MARGIN_X, 4, UI_CONTENT_W, 86,
                                 COL_CARD_BG, COL_CARD_BORDER, 4);
    lv_obj_t *bar_bg;
    lv_obj_t *rl;
    char buf[64];

    if (soc_pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", soc_pct);
        create_lbl(ec, font_20_mono(), COL_WHITE, 8, 4, 76, buf);
    } else {
        create_lbl(ec, font_16(), COL_WHITE, 8, 6, 76, pk_copy(PK_MSG_BATTERY_UNKNOWN));
    }

    if (f_range && f_range->has_value && f_range->unit_verified) {
        snprintf(buf, sizeof(buf), "EPA %d.%d km", f_range->value / 10, f_range->value % 10);
        rl = create_lbl(ec, font_12(), COL_MUTED, 88, 8, 128, buf);
    } else {
        rl = create_lbl(ec, font_12(), COL_MUTED, 88, 8, 128, pk_copy(PK_MSG_RANGE_UNIT_PENDING));
    }
    lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_RIGHT, 0);

    bar_bg = ui_create_box(ec, 8, 30, 208, 4, COL_BADGE_BG, 0, 2);
    if (soc_pct > 0) {
        int fw = (soc_pct > 100 ? 100 : soc_pct) * 208 / 100;
        (void)ui_create_box(bar_bg, 0, 0, fw, 4, COL_WHITE, 0, 2);
    }

    if (f_eta_full && f_eta_full->has_value) {
        snprintf(buf, sizeof(buf), "充满 %d:%02d",
                 f_eta_full->value / 60, f_eta_full->value % 60);
    } else if (strcmp(app->status_phase, "loading") == 0) {
        snprintf(buf, sizeof(buf), "正在读取车辆状态...");
    } else if (strcmp(app->status_phase, "offline") == 0) {
        snprintf(buf, sizeof(buf), "暂未连上车，连车后可重读");
    } else if (strcmp(app->status_phase, "timeout") == 0) {
        snprintf(buf, sizeof(buf), "读取超时，按●重读，双击●返回");
    } else {
        snprintf(buf, sizeof(buf), "充满 —");
    }
    create_lbl(ec, font_12(), COL_MUTED, 8, 39, 208, buf);

    create_icon(ec, APP_ICON_THERMOMETER, COL_MUTED, 6, 59);
    if (f_temp && f_temp->has_value) {
        int t10 = f_temp->value;
        int abs10 = (t10 >= 0) ? t10 : -t10;
        snprintf(buf, sizeof(buf), "座舱 %s%d.%d°C", (t10 < 0) ? "-" : "", abs10 / 10, abs10 % 10);
    } else {
        snprintf(buf, sizeof(buf), "座舱 —°C");
    }
    create_lbl(ec, font_12(), COL_WHITE, 28, 61, 188, buf);
}

static void render_status_closures_card(lv_obj_t *body, const pk_app_t *app)
{
    const pk_status_field_t *f_locked = find_status_field(app, "locked");
    lv_obj_t *cc = ui_create_box(body, UI_MARGIN_X, 94, UI_CONTENT_W, 28,
                                 COL_CARD_BG, COL_CARD_BORDER, 4);
    char buf[64];

    if (f_locked && f_locked->has_value) {
        snprintf(buf, sizeof(buf), "车锁 %s", f_locked->value ? "已锁" : "未锁");
    } else {
        snprintf(buf, sizeof(buf), "车锁 —");
    }
    create_lbl(cc, font_12(), COL_WHITE, 8, 6, 208, buf);
}

static void render_status_tires_card(lv_obj_t *body, const pk_app_t *app)
{
    const pk_status_field_t *f_fl = find_status_field(app, "tpms_fl");
    const pk_status_field_t *tires[4] = {
        f_fl,
        find_status_field(app, "tpms_fr"),
        find_status_field(app, "tpms_rl"),
        find_status_field(app, "tpms_rr"),
    };
    static const char *t_names[4] = { "左前", "右前", "左后", "右后" };
    lv_obj_t *tc = ui_create_box(body, UI_MARGIN_X, 126, UI_CONTENT_W, 88,
                                 COL_CARD_BG, COL_CARD_BORDER, 4);
    lv_obj_t *ul;
    char buf[32];
    int i;

    create_lbl(tc, font_12(), COL_MUTED, 8, 4, 100, "胎压监测");
    ul = create_lbl(tc, font_12(), COL_MUTED, 120, 4, 96, "单位 bar");
    lv_obj_set_style_text_align(ul, LV_TEXT_ALIGN_RIGHT, 0);

    for (i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        int tx = 8 + col * 108;
        int ty = 24 + row * 20;
        lv_obj_t *pl;
        create_lbl(tc, font_12(), COL_MUTED, tx, ty, 40, t_names[i]);
        if (tires[i] && tires[i]->has_value) {
            snprintf(buf, sizeof(buf), "%d.%d", tires[i]->value / 1000,
                     (tires[i]->value % 1000) / 100);
        } else {
            snprintf(buf, sizeof(buf), "—");
        }
        pl = create_lbl(tc, font_12(), COL_WHITE, tx + 44, ty, 52, buf);
        lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_RIGHT, 0);
    }
    /* Sample timeliness wording. */
    create_lbl(tc, font_12(), COL_MUTED, 8, 66, 208,
               (f_fl && f_fl->sample_present) ? "本次读窗（或为行驶前旧样）"
                                              : "采样时间未知，不冒充实时");
}

static void render_status_power_card(lv_obj_t *body)
{
    lv_obj_t *pc = ui_create_box(body, UI_MARGIN_X, 218, UI_CONTENT_W, 36,
                                 COL_CARD_BG, COL_CARD_BORDER, 4);
    lv_obj_t *pv;

    create_lbl(pc, font_12(), COL_WHITE, 8, 10, 80, "瞬时功率");
    pv = create_lbl(pc, font_12(), COL_MUTED, 92, 10, 124, pk_copy(PK_MSG_POWER_DASH));
    lv_obj_set_style_text_align(pv, LV_TEXT_ALIGN_RIGHT, 0);
}

void app_ui_render_page_status(lv_obj_t *body, const pk_app_t *app)
{
    render_status_energy_card(body, app);
    render_status_closures_card(body, app);
    render_status_tires_card(body, app);
    render_status_power_card(body);
}

void app_ui_render_page_vin(lv_obj_t *body, const pk_app_t *app)
{
    lv_obj_t *net, *qr_card, *qr_lbl, *vstat;
    char state_txt[32];
    char cnt_txt[16];


    create_lbl(body, font_16(), COL_WHITE, 8, 4, 224, "录入 17 位车架号");
    create_lbl(body, font_12(), COL_MUTED, 8, 26, 224, "连接设备热点并打开页提交");

    net = ui_create_box(body, UI_MARGIN_X, 46, UI_CONTENT_W, 42,
                        COL_CARD_BG, COL_CARD_BORDER, 4);
    create_lbl(net, font_12(), COL_MUTED, 8, 4, 90, "Wi-Fi 热点");
    {
        lv_obj_t *v1 = create_lbl(net, font_12(), COL_WHITE, 100, 4, 116, "PASSPORT-KEY");
        lv_obj_set_style_text_align(v1, LV_TEXT_ALIGN_RIGHT, 0);
    }
    create_lbl(net, font_12(), COL_MUTED, 8, 22, 90, "配置地址");
    {
        lv_obj_t *v2 = create_lbl(net, font_12(), COL_WHITE, 100, 22, 116, "192.168.4.1");
        lv_obj_set_style_text_align(v2, LV_TEXT_ALIGN_RIGHT, 0);
    }

    qr_card = ui_create_box(body, (240 - 104) / 2, 94, 104, 66, COL_CARD_BG, COL_CARD_BORDER, 4);
    qr_lbl = create_lbl_wrap(qr_card, font_12(), COL_WHITE, 6, 14, 92, "HTTP\n192.168.4.1");
    lv_obj_set_style_text_align(qr_lbl, LV_TEXT_ALIGN_CENTER, 0);

    create_icon(body, APP_ICON_CELLPHONE, COL_MUTED, 24, 164);
    create_lbl(body, font_12(), COL_MUTED, 48, 166, 184, "连热点打开 192.168.4.1");

    if (app->has_vin) {
        char masked[16];
        app_ports_format_masked_vin(app->vin, masked, sizeof(masked));
        snprintf(state_txt, sizeof(state_txt), "已录入 %s", masked);
        snprintf(cnt_txt, sizeof(cnt_txt), "17/17 位");
    } else if (app->message == PK_MSG_VIN_BAD) {
        snprintf(state_txt, sizeof(state_txt), "格式无效，请重填");
        snprintf(cnt_txt, sizeof(cnt_txt), "0/17 位");
    } else if (app->message == PK_MSG_AP_EXPIRED) {
        snprintf(state_txt, sizeof(state_txt), "热点超时，请重进");
        snprintf(cnt_txt, sizeof(cnt_txt), "0/17 位");
    } else {
        snprintf(state_txt, sizeof(state_txt), app->ap ? "等待手机提交" : "热点已关闭");
        snprintf(cnt_txt, sizeof(cnt_txt), "0/17 位");
    }

    vstat = ui_create_box(body, UI_MARGIN_X, 190, UI_CONTENT_W, 32,
                          COL_CARD_BG, COL_CARD_BORDER, 4);
    create_lbl(vstat, font_12(), (app->message == PK_MSG_VIN_BAD) ? COL_ALERT : COL_WHITE,
               8, 8, 142, state_txt);
    {
        lv_obj_t *cnt = create_lbl(vstat, font_12(), COL_MUTED, 150, 8, 66, cnt_txt);
        lv_obj_set_style_text_align(cnt, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

void app_ui_render_page_pair(lv_obj_t *body, const pk_app_t *app)
{
    static const char *s_titles[4] = {
        "核对实车与车架号",
        "原厂钥匙卡放读卡区",
        "等待车辆确认添加钥匙",
        "本机验证完成配对"
    };
    static const char *s_descs[4] = {
        "确认车架号与实车一致",
        "左无线充 / 水杯架一侧",
        "放原厂卡后等待车端批准",
        "完成安全认证并保存钥匙"
    };
    int i;
    int y = 5;
    int cur = app->pair_step ? app->pair_step : 1;
    int pair_state = app_ports_ble_pair_state(app);
    const char *msg = pk_copy(app->message);
    char step1_desc[48];

    if (app->has_vin) {
        char tail[16];
        app_ports_format_masked_vin(app->vin, tail, sizeof(tail));
        snprintf(step1_desc, sizeof(step1_desc), "目标车架号末尾 %s", tail);
    } else {
        snprintf(step1_desc, sizeof(step1_desc), "%s", s_descs[0]);
    }

    for (i = 1; i <= 4; i++) {
        bool on = (i == cur);
        bool done = (i < cur);
        lv_obj_t *card = ui_create_box(body, UI_MARGIN_X, y, UI_CONTENT_W, 44,
                                       on ? COL_ON_BG : COL_CARD_BG,
                                       on ? COL_ON_BORDER : COL_CARD_BORDER, 4);
        lv_obj_t *badge = ui_create_box(card, 8, 12, 20, 20, COL_BADGE_BG, 0, 10);
        char btxt[4];
        const char *desc = s_descs[i - 1];

        if (done) {
            create_icon(badge, APP_ICON_CHECK_CIRCLE, COL_WHITE, 0, 0);
        } else {
            lv_obj_t *bl;
            snprintf(btxt, sizeof(btxt), "%d", i);
            bl = create_lbl(badge, font_12(), on ? COL_WHITE : COL_MUTED, 0, 2, 20, btxt);
            lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
        }

        if (i == 1) {
            desc = step1_desc;
        } else if (i == 2 && cur == 2) {
            desc = (pair_state == 1) ? "车辆蓝牙已连，放卡后按●"
                 : (pair_state == 2) ? "请求已发，请放卡确认"
                                     : "正在连接车辆蓝牙，先放卡";
        } else if (i == 3 && cur == 3) {
            desc = (pair_state == 2) ? "已发添加请求，请在车机确认"
                 : (pair_state == 1) ? "蓝牙已连，正在发送请求"
                                     : "正在连接车辆蓝牙...";
        }

        create_lbl(card, font_12(), COL_WHITE, 34, 6, 182, s_titles[i - 1]);
        create_lbl(card, font_12(), COL_MUTED, 34, 24, 182, desc);
        y += 48;
    }

    {
        lv_obj_t *nc = ui_create_box(body, UI_MARGIN_X, y, UI_CONTENT_W, 54,
                                     COL_CARD_BG, COL_CARD_BORDER, 4);
        (void)create_lbl_wrap(nc, font_12(), COL_MUTED, 8, 5, 208,
                              (msg && msg[0])
                                  ? msg
                                  : "第2步下一步仅发送一次登记请求；第4步验证后才保存。");
    }
}

void app_ui_render_page_offline(lv_obj_t *body, const pk_app_t *app)
{
    const char *msg = pk_copy(app->message);
    bool drive_warn = (app->moving || app->auth == PK_MSG_AUTH_UNCONFIRMED);
    build_glance_card(body, 4, 66, drive_warn ? APP_ICON_ALERT : APP_ICON_BT_OFF, drive_warn,
                      drive_warn ? "驾驶中蓝牙断开" : "请先重连",
                      drive_warn ? "勿依赖本设备，注意安全"
                                 : ((msg && msg[0]) ? msg : "车控已禁用，不排队补发"));
    build_zone_card(body, 75, 52, false, false, true, "前备箱", "长按 ▲", "请先重连");
    build_zone_card(body, 132, 52, true, false, false, "重新连接",
                    app->reconnect.active ? "正在有界扫描..." : "发起有界扫描连接", "单击 ●");
    build_zone_card(body, 189, 52, false, false, true, "后备箱", "长按 ▼", "请先重连");
}

void app_ui_render_page_wait_or_result(lv_obj_t *body, const pk_app_t *app)
{
    pk_feedback_state_t feedback = pk_feedback_state(app);
    bool is_wait = (feedback == PK_FEEDBACK_PROGRESS);
    const char *msg = app->message_extra[0] ? app->message_extra : pk_copy(app->message);
    bool is_alert = (feedback == PK_FEEDBACK_FAILURE);
    bool is_unverified = (feedback == PK_FEEDBACK_UNVERIFIED);
    app_icon_id_t ico = is_wait ? APP_ICON_REFRESH
                                : (is_alert ? APP_ICON_ALERT
                                            : (is_unverified ? APP_ICON_CLIPBOARD
                                                             : APP_ICON_CHECK_CIRCLE));
    const char *gt = is_wait ? "正在发送指令"
                             : (is_alert ? "请检查车辆"
                                         : (is_unverified ? "请求已发送，待确认" : "操作已完成"));
    const char *gs = is_wait ? "正在等待车辆响应"
                             : (app->result_auto_return ? "2 秒后自动返回主页" : "按●返回主页");

    if (app->lock_fsm == PK_LOCK_FSM_VERIFY) {
        gt = "正在确认锁车";
        gs = "正在读取车门锁状态...";
    } else if (app->lock_fsm == PK_LOCK_FSM_RESULT) {
        gt = (app->message == PK_MSG_LOCK_CONFIRMED) ? "已锁车" : "请检查车辆";
    }

    build_glance_card(body, 4, 66, ico, is_alert, gt, gs);

    {
        bool red_alert = (!is_wait && (app->lock_alert_active || is_alert));
        lv_obj_t *rc = ui_create_box(body, UI_MARGIN_X, 76, UI_CONTENT_W, 96,
                                     red_alert ? COL_ALERT : COL_CARD_BG,
                                     red_alert ? COL_WHITE : COL_CARD_BORDER, 4);
        create_lbl(rc, font_12(), red_alert ? COL_WHITE : COL_MUTED, 8, 6, 208,
                   red_alert ? "请立即检查车辆" : "状态结果");
        (void)create_lbl_wrap(rc, red_alert ? font_16() : font_12(), COL_WHITE, 8, 26, 208,
                              (msg && msg[0]) ? msg : "请查看车辆实际状态");
    }

    build_zone_card(body, 180, 48, true, false, false,
                    is_wait ? "取消等待" : "返回主页",
                    is_wait ? "停止等待并返回" : "按●立即返回",
                    "单击 ●");
}

void app_ui_render_page_dialog(lv_obj_t *body, const pk_app_t *app)
{
    const char *gt = "确认操作";
    const char *gs = "默认停在取消，防止误操作";
    const char *opt0 = "取消（默认）";
    const char *opt1 = "确认";
    bool focus_confirm = false;
    bool alert_confirm = false;

    if (app->page == PK_PAGE_QUALIFICATION_OFFER) {
        bool wake_mode = (app->offer_focus == PK_OFFER_WAKE || app->offer_focus == PK_OFFER_BACK);
        gt = wake_mode ? "车辆可能休眠" : "驻车资格已过期";
        gs = pk_copy(app->message);
        opt0 = wake_mode ? "唤醒车辆并检查" : "立即检查驻车资格";
        opt1 = wake_mode ? "返回（不唤醒）" : "取消草稿并返回";
        focus_confirm = (app->offer_focus == PK_OFFER_CANCEL || app->offer_focus == PK_OFFER_BACK);
        build_glance_card(body, 4, 66, APP_ICON_ALERT, true, gt, gs);
        build_zone_card(body, 76, 54, !focus_confirm, false, false,
                        opt0, "单次请求，不自动重发", "●");
        build_zone_card(body, 138, 54, focus_confirm, false, false,
                        opt1, "不唤醒，不补发旧操作", "●");
        return;
    }

    if (app->page == PK_PAGE_DELETE_CONFIRM) {
        gt = "删除本机车辆记录";
        gs = "仅删除本机记录，不会撤销车端钥匙登记";
        opt0 = "取消（保留记录）";
        opt1 = "确认删除本机记录";
        focus_confirm = (app->delete_focus == 1);
        alert_confirm = true;
    } else if (app->page == PK_PAGE_RESET_CONFIRM) {
        gt = "恢复本机出厂设置";
        gs = "清空本机车辆与设置，不撤销车端登记";
        opt0 = "取消（默认）";
        opt1 = "确认恢复出厂";
        focus_confirm = (app->reset_focus == 1);
        alert_confirm = true;
    }

    build_glance_card(body, 4, 66, APP_ICON_ALERT, alert_confirm, gt, gs);
    build_zone_card(body, 76, 54, !focus_confirm, false, false, opt0, "推荐默认项", "●");
    build_zone_card(body, 138, 54, focus_confirm, alert_confirm, false, opt1, "二次确认后执行", "●");
}

void app_ui_render_page_logs(lv_obj_t *body, const pk_app_t *app)
{
    char line[64];
    unsigned cnt = app->logs.count;

    build_glance_card(body, 4, 66, APP_ICON_CLIPBOARD, false,
                      "本机脱敏日志",
                      "不含VIN/密钥/PIN/MAC");

    {
        lv_obj_t *lc = ui_create_box(body, UI_MARGIN_X, 76, UI_CONTENT_W, 118,
                                     COL_CARD_BG, COL_CARD_BORDER, 4);

        snprintf(line, sizeof(line), "记录总数：%u / 64 条", cnt);
        create_lbl(lc, font_12(), COL_MUTED, 8, 6, 208, line);

        if (cnt == 0) {
            create_lbl(lc, font_12(), COL_WHITE, 8, 32, 208, "暂无运行事件记录");
        } else {
            unsigned idx = (unsigned)app->log_focus % cnt;
            pk_log_record_t rec;
            if (pk_log_get(&app->logs, idx, &rec)) {
                snprintf(line, sizeof(line), "#%u  事件码:%d  结果码:%d",
                         idx + 1, (int)rec.event, (int)rec.error);
                create_lbl(lc, font_12(), COL_WHITE, 8, 32, 208, line);
                snprintf(line, sizeof(line), "时刻：%u.%u s · %s",
                         (unsigned)(rec.elapsed_ms / 1000u),
                         (unsigned)((rec.elapsed_ms % 1000u) / 100u),
                         rec.slot_current ? "当前车" : "无车");
                create_lbl(lc, font_12(), COL_MUTED, 8, 54, 208, line);
            }
        }
    }

    build_zone_card(body, 200, 46, true, false, false, "返回设置", NULL, "●");
}
