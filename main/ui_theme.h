#ifndef UI_THEME_H
#define UI_THEME_H

#include "app_font_cjk.h"
#include "lvgl.h"

/* ==========================================================================
 * Stitch "Tesla Bluetooth Key GUI" 240x320 Dark Instrument Palette
 * ========================================================================== */
#define COL_BG          0x050507
#define COL_BAR_BG      0x090A0F
#define COL_BAR_BORDER  0x16181F
#define COL_FOOT_BG     0x0A0B10
#define COL_FOOT_BORDER 0x181B24
#define COL_CARD_BG     0x0C0D12
#define COL_CARD_BORDER 0x181B24
#define COL_ON_BG       0x11131C
#define COL_ON_BORDER   0x8A90A2
#define COL_GICO_BG     0x0E1017
#define COL_GICO_BORDER 0x232733
#define COL_ALERT_BG    0x1A0B0C
#define COL_ALERT_BADGE 0x2D0D10
#define COL_ALERT_TEXT  0xFF8F94
#define COL_BADGE_BG    0x171A24
#define COL_BADGE_BDR   0x262B3A
#define COL_TAG_BORDER  0x3A4050
#define COL_QR_BG       0xFFFFFF
#define COL_WHITE       0xEEF2F8
#define COL_MUTED       0x8A90A2
#define COL_ALERT       0xE82127
#define COL_ARC_TRACK   0x232733
#define COL_ARC_IND     0xC8CDD8

#define UI_MARGIN_X     8
#define UI_CONTENT_W    224

static inline const lv_font_t *font_12(void)
{
    return app_font_cjk_12();
}

static inline const lv_font_t *font_16(void)
{
    return app_font_cjk_16();
}

static inline const lv_font_t *font_20_mono(void)
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return app_font_cjk_16();
#endif
}

#endif /* UI_THEME_H */
