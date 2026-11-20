#pragma once

#include "pk_app.h"
#include <stdbool.h>

/*
 * Stitch "Tesla Bluetooth Key GUI" 240x320 dark instrument renderer:
 *   - Exact 240x320 layout: 22px top bar (#090A0F), 262px body (#050507), 36px footer (#0A0B10)
 *   - Plain-text / licensed symbol badges in icon slots (app_icons.h, P2 Gate-B compliant)
 *   - Custom 12px & 16px CJK + symbol fonts (app_font_cjk.h) with 100% glyph coverage
 *   - Zero touch, zero blur/animation, #E82127 strictly reserved for unlock & real alerts
 */
void app_ui_init(void);
bool app_ui_needs_update(const pk_app_t *app);
void app_ui_update(const pk_app_t *app);
