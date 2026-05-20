#include "ui_font.h"

void ui_font_init(void)
{
}

const lv_font_t *ui_font_normal(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *ui_font_small(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#elif LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return LV_FONT_DEFAULT;
#endif
}
