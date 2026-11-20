#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "app_icons.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

void style_reset_box(lv_obj_t *obj, uint32_t bg_hex, uint32_t border_hex, int radius, int pad);
lv_obj_t *ui_create_box(lv_obj_t *parent, int x, int y, int w, int h,
                        uint32_t bg_hex, uint32_t border_hex, int radius);

lv_obj_t *create_icon(lv_obj_t *parent, app_icon_id_t id, uint32_t hex_color, int x, int y);
void update_icon(lv_obj_t *img, app_icon_id_t id, uint32_t hex_color);

int text_px_width(const lv_font_t *f, const char *s);

lv_obj_t *create_lbl(lv_obj_t *parent, const lv_font_t *f, uint32_t hex_color,
                     int x, int y, int w, const char *txt);
lv_obj_t *create_lbl_wrap(lv_obj_t *parent, const lv_font_t *f, uint32_t hex_color,
                          int x, int y, int w, const char *txt);

lv_obj_t *build_banner(lv_obj_t *parent, int y, app_icon_id_t ico, const char *txt);
lv_obj_t *build_glance_card(lv_obj_t *parent, int y, int h,
                            app_icon_id_t ico, bool is_alert,
                            const char *title, const char *sub);
lv_obj_t *build_zone_card(lv_obj_t *parent, int y, int h,
                          bool focused, bool is_alert, bool disabled,
                          const char *title, const char *desc, const char *badge);

#endif /* UI_WIDGETS_H */
