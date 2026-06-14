#include "ui_font.h"

#ifdef APP_USE_CUSTOM_UI_FONTS
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_16);
#endif

void ui_font_init(void)
{
}

const lv_font_t *ui_font_normal(void)
{
#ifdef APP_USE_CUSTOM_UI_FONTS
    return &ui_font_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *ui_font_small(void)
{
#ifdef APP_USE_CUSTOM_UI_FONTS
    return &ui_font_14;
#else
    return LV_FONT_DEFAULT;
#endif
}
