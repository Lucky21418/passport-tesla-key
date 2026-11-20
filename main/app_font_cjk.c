#include "app_font_cjk.h"

/*
 * Thin application font wrappers backed by the licensed Source Han Sans SC
 * Regular (OFL-1.1) subsets generated into assets/fonts/ via official
 * lv_font_conv.
 */
LV_FONT_DECLARE(app_font_source_han_sans_sc_12);
LV_FONT_DECLARE(app_font_source_han_sans_sc_16);

const lv_font_t *app_font_cjk_12(void)
{
    return &app_font_source_han_sans_sc_12;
}

const lv_font_t *app_font_cjk_16(void)
{
    return &app_font_source_han_sans_sc_16;
}
