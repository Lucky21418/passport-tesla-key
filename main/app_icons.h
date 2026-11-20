#pragma once
#include "lvgl.h"

/*
 * P2 Adjudication (Gate-B):
 * The 31 unlicensed bitmap icons and QR bitmap from the legacy tree have been
 * stripped and are prohibited from entering this repository.
 * UI icon slots use plain-text / licensed LVGL symbol labels instead of
 * embedded bitmap descriptors.
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
    APP_ICON_TESLA_FAN,
    APP_ICON_TESLA_SENTRY,
    APP_ICON_TESLA_PORT,
    APP_ICON_TESLA_VENT,
    APP_ICON_TESLA_FLASH,
    APP_ICON_TESLA_HORN,
    APP_ICON_ALERT,
    APP_ICON_CHECK_CIRCLE,
    APP_ICON_PLUS,
    APP_ICON_CLIPBOARD,
    APP_ICON_COG,
    APP_ICON_KEY,
    APP_ICON_MOON,
    APP_ICON_BRIGHTNESS,
    APP_ICON_POWER_PLUG,
    APP_ICON_SEAT_HEATER,
    APP_ICON_STEERING,
    APP_ICON_THERMOMETER,
    APP_ICON_WIFI,
    APP_ICON_TRASH,
    APP_ICON_CELLPHONE,
    APP_ICON_REFRESH,
    APP_ICON_COUNT
} app_icon_id_t;

const char *app_icon_text(app_icon_id_t id);
