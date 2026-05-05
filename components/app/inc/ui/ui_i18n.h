#ifndef UI_I18N_H
#define UI_I18N_H

#include <stdbool.h>

typedef enum {
    UI_LANG_CN = 0,
    UI_LANG_EN
} ui_lang_t;

typedef enum {
    UI_TEXT_APP_INTERCOM = 0,
    UI_TEXT_APP_CAMERA,
    UI_TEXT_APP_AI,
    UI_TEXT_APP_SETTINGS,
    UI_TEXT_INTERCOM_PTT,
    UI_TEXT_INTERCOM_CHANNEL,
    UI_TEXT_CAMERA_LIVE,
    UI_TEXT_CAMERA_LOCKED,
    UI_TEXT_CAMERA_CAPTURE,
    UI_TEXT_CAMERA_UPLOAD,
    UI_TEXT_CAMERA_RETAKE,
    UI_TEXT_AI_IDLE,
    UI_TEXT_AI_LISTENING,
    UI_TEXT_AI_ASK,
    UI_TEXT_SETTINGS_BRIGHTNESS,
    UI_TEXT_SETTINGS_VOLUME,
    UI_TEXT_SETTINGS_LANGUAGE,
    UI_TEXT_SETTINGS_SERVER,
    UI_TEXT_SETTINGS_IP,
    UI_TEXT_SETTINGS_PORT,
    UI_TEXT_COUNT
} ui_text_id_t;

/*
 * Windows simulator note:
 *   The language state is fully handled here. If Chinese glyphs appear as
 *   boxes, enable or add a CJK font in lv_conf.h and apply it to labels.
 *
 * ESP32-S3 LCD porting note:
 *   Keep this i18n API unchanged. Replace the simulator font with a generated
 *   LVGL CJK font, store it in flash/PSRAM as appropriate, and apply it from
 *   the board UI theme/init code.
 */
void ui_i18n_set_language(ui_lang_t lang);
ui_lang_t ui_i18n_get_language(void);
bool ui_i18n_is_english(void);
const char * ui_i18n_text(ui_text_id_t id);

#endif /* UI_I18N_H */
