#include "ui_widgets.h"
#include "ui_theme.h"

const char *app_icon_text(app_icon_id_t id)
{
    switch (id) {
    case APP_ICON_BT_CONNECT:   return "BT";
    case APP_ICON_BT_OFF:       return "--";
    case APP_ICON_BATT_60:      return "60";
    case APP_ICON_BATT_10:      return "10";
    case APP_ICON_TESLA_LOCK:   return "锁";
    case APP_ICON_LOCK_OPEN:    return "开";
    case APP_ICON_TESLA_CAR:    return "车";
    case APP_ICON_TESLA_HOOD:   return "前";
    case APP_ICON_TESLA_TRUNK:  return "后";
    case APP_ICON_TESLA_FAN:    return "风";
    case APP_ICON_TESLA_SENTRY: return "哨";
    case APP_ICON_TESLA_PORT:   return "充";
    case APP_ICON_TESLA_VENT:   return "窗";
    case APP_ICON_TESLA_FLASH:  return "灯";
    case APP_ICON_TESLA_HORN:   return "笛";
    case APP_ICON_ALERT:        return "!";
    case APP_ICON_CHECK_CIRCLE: return "OK";
    case APP_ICON_PLUS:         return "+";
    case APP_ICON_CLIPBOARD:    return "状";
    case APP_ICON_COG:          return "设";
    case APP_ICON_KEY:          return "钥";
    case APP_ICON_MOON:         return "眠";
    case APP_ICON_BRIGHTNESS:   return "亮";
    case APP_ICON_POWER_PLUG:   return "电";
    case APP_ICON_SEAT_HEATER:  return "座";
    case APP_ICON_STEERING:     return "盘";
    case APP_ICON_THERMOMETER:  return "°C";
    case APP_ICON_WIFI:         return "AP";
    case APP_ICON_TRASH:        return "删";
    case APP_ICON_CELLPHONE:    return "机";
    case APP_ICON_REFRESH:      return "..";
    default:                    return "●";
    }
}

void style_reset_box(lv_obj_t *obj, uint32_t bg_hex, uint32_t border_hex, int radius, int pad)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_hex), 0);
    lv_obj_set_style_border_width(obj, border_hex ? 1 : 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, pad, 0);
    lv_obj_set_style_text_font(obj, font_12(), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(COL_WHITE), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_create_box(lv_obj_t *parent, int x, int y, int w, int h,
                        uint32_t bg_hex, uint32_t border_hex, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    style_reset_box(box, bg_hex, border_hex, radius, 0);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

lv_obj_t *create_icon(lv_obj_t *parent, app_icon_id_t id, uint32_t hex_color, int x, int y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, app_icon_dsc(id));
    lv_obj_set_style_image_recolor(img, lv_color_hex(hex_color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_size(img, 20, 20);
    return img;
}

void update_icon(lv_obj_t *ico_obj, app_icon_id_t id, uint32_t hex_color)
{
    lv_image_set_src(ico_obj, app_icon_dsc(id));
    lv_obj_set_style_image_recolor(ico_obj, lv_color_hex(hex_color), 0);
    lv_obj_set_style_image_recolor_opa(ico_obj, LV_OPA_COVER, 0);
}

int text_px_width(const lv_font_t *f, const char *s)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (p && *p) {
        uint32_t cp = 0;
        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xE0u) == 0xC0u && p[1]) {
            cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0Fu) << 12) |
                 ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
            p += 3;
        } else if ((*p & 0xF8u) == 0xF0u && p[1] && p[2] && p[3]) {
            cp = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
                 ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
            p += 4;
        } else {
            p++;
            continue;
        }
        w += (int)lv_font_get_glyph_width(f, cp, 0);
    }
    return w;
}

lv_obj_t *create_lbl(lv_obj_t *parent, const lv_font_t *f, uint32_t hex_color,
                     int x, int y, int w, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex_color), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_obj_set_height(l, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *create_lbl_wrap(lv_obj_t *parent, const lv_font_t *f, uint32_t hex_color,
                          int x, int y, int w, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex_color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *build_banner(lv_obj_t *parent, int y, app_icon_id_t ico, const char *txt)
{
    lv_obj_t *box = ui_create_box(parent, UI_MARGIN_X, y, UI_CONTENT_W, 26,
                                  COL_ALERT_BG, COL_ALERT, 4);
    create_icon(box, ico, COL_ALERT, 6, 2);
    create_lbl(box, font_12(), COL_WHITE, 30, 5, 188, txt);
    return box;
}

lv_obj_t *build_glance_card(lv_obj_t *parent, int y, int h,
                            app_icon_id_t ico, bool is_alert,
                            const char *title, const char *sub)
{
    lv_obj_t *card = ui_create_box(parent, UI_MARGIN_X, y, UI_CONTENT_W, h,
                                   COL_CARD_BG, COL_CARD_BORDER, 4);
    lv_obj_t *gico = ui_create_box(card, 8, (h - 38) / 2, 38, 38,
                                   is_alert ? COL_ALERT_BG : COL_GICO_BG,
                                   is_alert ? COL_ALERT : COL_GICO_BORDER, 6);
    create_icon(gico, ico, is_alert ? COL_ALERT : COL_WHITE, 8, 8);

    create_lbl(card, font_16(), COL_WHITE, 54,
               sub && sub[0] ? (h >= 66 ? 5 : 7) : ((h - 18) / 2), 164, title);
    if (sub && sub[0]) {
        if (h >= 66) {
            create_lbl_wrap(card, font_12(), COL_MUTED, 54, 27, 164, sub);
        } else {
            create_lbl(card, font_12(), COL_MUTED, 54, 29, 164, sub);
        }
    }
    return card;
}

lv_obj_t *build_zone_card(lv_obj_t *parent, int y, int h,
                          bool focused, bool is_alert, bool disabled,
                          const char *title, const char *desc, const char *badge)
{
    uint32_t bg = focused ? COL_ON_BG : COL_CARD_BG;
    uint32_t bdr = focused ? (is_alert ? COL_ALERT : COL_ON_BORDER) : COL_CARD_BORDER;
    uint32_t ttl_col = disabled ? COL_MUTED : COL_WHITE;
    lv_obj_t *card = ui_create_box(parent, UI_MARGIN_X, y, UI_CONTENT_W, h, bg, bdr, 4);
    int bw = 0;

    if (badge && badge[0]) {
        bw = text_px_width(font_12(), badge) + 14;
        if (bw < 36) {
            bw = 36;
        }
        if (bw > 118) {
            bw = 118;
        }
    }
    {
        int left_w = bw ? (UI_CONTENT_W - bw - 22) : 208;
        if (desc && desc[0]) {
            create_lbl(card, font_12(), ttl_col, 8, 4, left_w, title);
            create_lbl(card, font_12(), COL_MUTED, 8, 22, left_w, desc);
        } else {
            create_lbl(card, font_12(), ttl_col, 8, (h - 16) / 2, left_w, title);
        }
    }

    if (bw > 0) {
        uint32_t zb_bg = (focused && is_alert) ? COL_ALERT_BADGE : COL_BADGE_BG;
        uint32_t zb_bdr = (focused && is_alert) ? COL_ALERT : (focused ? COL_ON_BORDER : COL_BADGE_BDR);
        uint32_t zb_txt = (focused && is_alert) ? COL_ALERT_TEXT : (focused ? COL_WHITE : COL_MUTED);
        lv_obj_t *zb = ui_create_box(card, UI_CONTENT_W - bw - 8, (h - 20) / 2, bw, 20,
                                     zb_bg, zb_bdr, 3);
        lv_obj_t *bl = create_lbl(zb, font_12(), zb_txt, 0, 1, bw - 2, badge);
        lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
    }
    return card;
}
