#pragma once
#include "lvgl.h"

/*
 * Official CBI (custom-brand-icons, MIT License) and MDI (@mdi/svg,
 * Pictogrammers, Apache-2.0 License) 20x20 LV_COLOR_FORMAT_A8 icon bitmaps
 * and 58x58 Project Nayuki qrcodegen (MIT License) QR bitmap.
 *
 * Source SVGs and license texts are tracked in assets/icons/:
 *   - assets/icons/MIT-CBI.txt
 *   - assets/icons/Apache-2.0-MDI.txt
 *   - assets/icons/cbi/ (4 official CBI SVGs)
 *   - assets/icons/mdi/ (17 official MDI SVGs)
 * Reproducibly rasterized via tools/build_app_icons.py (rsvg-convert + convert).
 */
typedef enum {
    APP_ICON_BT_CONNECT,
    APP_ICON_BT_OFF,
    APP_ICON_BATT_60,
    APP_ICON_BATT_10,
    APP_ICON_TESLA_LOCK,
    APP_ICON_LOCK_OPEN,
    APP_ICON_TESLA_CAR,
    APP_ICON_TESLA_HOOD,
    APP_ICON_TESLA_TRUNK,
    APP_ICON_ALERT,
    APP_ICON_CHECK_CIRCLE,
    APP_ICON_PLUS,
    APP_ICON_CLIPBOARD,
    APP_ICON_COG,
    APP_ICON_MOON,
    APP_ICON_BRIGHTNESS,
    APP_ICON_THERMOMETER,
    APP_ICON_WIFI,
    APP_ICON_TRASH,
    APP_ICON_CELLPHONE,
    APP_ICON_REFRESH,
    APP_ICON_COUNT
} app_icon_id_t;

const char *app_icon_text(app_icon_id_t id);
const lv_image_dsc_t *app_icon_dsc(app_icon_id_t id);
const lv_image_dsc_t *app_vin_qr_dsc(void);
