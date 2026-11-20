#ifndef APP_UI_INTERNAL_H
#define APP_UI_INTERNAL_H

#include "lvgl.h"
#include "pk_app_internal.h"
#include <stdbool.h>

typedef struct {
    lv_obj_t       *body;
    lv_obj_t       *hold_arc;
    lv_obj_t       *hold_time_lbl;
    pk_action_t     hold_rendered_act;
    lv_obj_t       *settings_rows[7];
    pk_setting_id_t settings_row_ids[7];
    int             settings_row_count;
    int             settings_rendered_start;
    bool            settings_rendered_quick;
    pk_msg_id_t     settings_rendered_msg;
} app_ui_ctx_t;

int app_ui_settings_window_start(int focus);

void app_ui_render_hold_gauge(app_ui_ctx_t *ctx, pk_action_t act,
                              pk_ms_t elapsed, pk_ms_t total, const char *cancel_hint);
void app_ui_render_page_main(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_cars(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_settings(app_ui_ctx_t *ctx, const pk_app_t *app);
void app_ui_render_page_status(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_vin(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_pair(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_offline(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_wait_or_result(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_dialog(lv_obj_t *body, const pk_app_t *app);
void app_ui_render_page_logs(lv_obj_t *body, const pk_app_t *app);

#endif /* APP_UI_INTERNAL_H */
