#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"
#include "ui_shell.h"

#define UI_COLOR_INTERCOM  lv_color_make(0xFF, 0x66, 0x00)
#define UI_COLOR_AI        lv_color_make(0x10, 0xD0, 0xFF)
#define UI_COLOR_SETTINGS  lv_color_make(0xB8, 0xB8, 0xB8)

static inline lv_color_t ui_theme_app_color(ui_app_id_t app)
{
    switch(app) {
        case UI_APP_ID_INTERCOM:
            return UI_COLOR_INTERCOM;
        case UI_APP_ID_AI:
            return UI_COLOR_AI;
        case UI_APP_ID_SETTINGS:
            return UI_COLOR_SETTINGS;
        default:
            return lv_color_white();
    }
}

#endif /* UI_THEME_H */
