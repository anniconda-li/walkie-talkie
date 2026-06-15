#include "ui_font.h"

LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_16);

void ui_font_init(void)
{
}

const lv_font_t *ui_font_normal(void)
{
    return &ui_font_16;
}

const lv_font_t *ui_font_small(void)
{
    return &ui_font_14;
}
