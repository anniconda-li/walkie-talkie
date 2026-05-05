#include "ui_event.h"
#include "ui_i18n.h"
#include "ui_shell.h"
#include <string.h>

#define INTERCOM_CHANNEL_MIN 1
#define INTERCOM_CHANNEL_MAX 32
#define PTT_RING_BASE_SIZE 48
#define PTT_RING_MAX_SIZE 132
#define AI_BAR_BASE_H 12

static ui_event_callbacks_t g_callbacks;

static void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if(obj == NULL) {
        return;
    }

    if(hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_intercom_channel(ui_intercom_view_t *view)
{
    char text[16];

    if(view == NULL || view->channel_label == NULL) {
        return;
    }

    lv_snprintf(text, sizeof(text), "CH %02ld", (long)view->channel);
    lv_label_set_text(view->channel_label, text);
}

static void anim_set_ring_size(void *obj, int32_t size)
{
    lv_obj_set_size((lv_obj_t *)obj, size, size);
    lv_obj_center((lv_obj_t *)obj);
}

static void anim_set_opa(void *obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static void anim_set_ai_bar_h(void *obj, int32_t h)
{
    lv_obj_set_height((lv_obj_t *)obj, h);
}

static void start_ptt_ring_anim(lv_obj_t *ring, uint32_t delay)
{
    lv_anim_t size_anim;
    lv_anim_t opa_anim;

    if(ring == NULL) {
        return;
    }

    lv_anim_del(ring, anim_set_ring_size);
    lv_anim_del(ring, anim_set_opa);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ring, LV_OPA_COVER, 0);
    anim_set_ring_size(ring, PTT_RING_BASE_SIZE);

    lv_anim_init(&size_anim);
    lv_anim_set_var(&size_anim, ring);
    lv_anim_set_exec_cb(&size_anim, anim_set_ring_size);
    lv_anim_set_values(&size_anim, PTT_RING_BASE_SIZE, PTT_RING_MAX_SIZE);
    lv_anim_set_duration(&size_anim, 900);
    lv_anim_set_delay(&size_anim, delay);
    lv_anim_set_repeat_count(&size_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&size_anim, lv_anim_path_ease_out);
    lv_anim_start(&size_anim);

    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, ring);
    lv_anim_set_exec_cb(&opa_anim, anim_set_opa);
    lv_anim_set_values(&opa_anim, LV_OPA_COVER, LV_OPA_0);
    lv_anim_set_duration(&opa_anim, 900);
    lv_anim_set_delay(&opa_anim, delay);
    lv_anim_set_repeat_count(&opa_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_start(&opa_anim);
}

static void stop_ptt_ring_anim(lv_obj_t *ring, int32_t index)
{
    int32_t size = 58 + index * 24;

    if(ring == NULL) {
        return;
    }

    lv_anim_del(ring, anim_set_ring_size);
    lv_anim_del(ring, anim_set_opa);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ring, (lv_opa_t)(100 - index * 24), 0);
    anim_set_ring_size(ring, size);
}

static void set_intercom_talking(ui_intercom_view_t *view, bool talking)
{
    if(view == NULL) {
        return;
    }

    for(int32_t i = 0; i < 3; i++) {
        if(talking) {
            start_ptt_ring_anim(view->broadcast_rings[i], (uint32_t)i * 240);
        }
        else {
            stop_ptt_ring_anim(view->broadcast_rings[i], i);
        }
    }

    set_obj_hidden(view->channel_label, talking);
    set_obj_hidden(view->channel_hint_label, talking);

    if(view->ptt_button != NULL) {
        lv_obj_set_style_bg_color(view->ptt_button,
                                  talking ? lv_color_make(0xFF, 0x66, 0x00) : lv_color_make(0x3A, 0x3A, 0x3A),
                                  0);
    }
}

static void intercom_change_channel(ui_intercom_view_t *view, int32_t delta)
{
    if(view == NULL) {
        return;
    }

    view->channel += delta;
    if(view->channel < INTERCOM_CHANNEL_MIN) {
        view->channel = INTERCOM_CHANNEL_MIN;
    }
    if(view->channel > INTERCOM_CHANNEL_MAX) {
        view->channel = INTERCOM_CHANNEL_MAX;
    }

    refresh_intercom_channel(view);

    if(g_callbacks.intercom_channel_changed != NULL) {
        g_callbacks.intercom_channel_changed(view->channel);
    }
}

static void intercom_dec_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        intercom_change_channel((ui_intercom_view_t *)lv_event_get_user_data(e), -1);
    }
}

static void intercom_inc_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        intercom_change_channel((ui_intercom_view_t *)lv_event_get_user_data(e), 1);
    }
}

static void intercom_ptt_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ui_intercom_view_t *view = (ui_intercom_view_t *)lv_event_get_user_data(e);

    if(code == LV_EVENT_LONG_PRESSED) {
        set_intercom_talking(view, true);
        if(g_callbacks.intercom_ptt_started != NULL && view != NULL) {
            g_callbacks.intercom_ptt_started(view->channel);
        }
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        set_intercom_talking(view, false);
        if(g_callbacks.intercom_ptt_stopped != NULL && view != NULL) {
            g_callbacks.intercom_ptt_stopped(view->channel);
        }
    }
}

static void camera_capture_event_cb(lv_event_t *e)
{
    ui_camera_view_t *view = (ui_camera_view_t *)lv_event_get_user_data(e);

    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL) {
        return;
    }

    view->frozen = true;
    lv_obj_set_style_bg_color(view->preview, lv_color_make(0x24, 0x2E, 0x38), 0);
    lv_label_set_text(view->status_label, ui_i18n_text(UI_TEXT_CAMERA_LOCKED));
    lv_obj_add_state(view->capture_button, LV_STATE_DISABLED);
    lv_obj_remove_state(view->upload_button, LV_STATE_DISABLED);
    lv_obj_remove_state(view->retake_button, LV_STATE_DISABLED);

    if(g_callbacks.camera_capture_requested != NULL) {
        g_callbacks.camera_capture_requested();
    }
}

static void camera_upload_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && g_callbacks.camera_upload_requested != NULL) {
        g_callbacks.camera_upload_requested();
    }
}

static void camera_retake_event_cb(lv_event_t *e)
{
    ui_camera_view_t *view = (ui_camera_view_t *)lv_event_get_user_data(e);

    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL) {
        return;
    }

    view->frozen = false;
    lv_obj_set_style_bg_color(view->preview, lv_color_make(0x13, 0x1A, 0x22), 0);
    lv_label_set_text(view->status_label, ui_i18n_text(UI_TEXT_CAMERA_LIVE));
    lv_obj_remove_state(view->capture_button, LV_STATE_DISABLED);
    lv_obj_add_state(view->upload_button, LV_STATE_DISABLED);
    lv_obj_add_state(view->retake_button, LV_STATE_DISABLED);

    if(g_callbacks.camera_retake_requested != NULL) {
        g_callbacks.camera_retake_requested();
    }
}

static void ai_set_speaking(ui_ai_view_t *view, bool speaking)
{
    if(view == NULL) {
        return;
    }

    view->speaking = speaking;
    lv_obj_set_style_bg_color(view->ask_button,
                              speaking ? lv_color_make(0xFF, 0x66, 0x00) : lv_color_make(0x36, 0x36, 0x36),
                              0);
    for(int32_t i = 0; i < 4; i++) {
        lv_obj_t *bar = view->voice_bars[i];

        if(bar == NULL) {
            continue;
        }

        lv_anim_del(bar, anim_set_ai_bar_h);

        if(speaking) {
            lv_anim_t anim;
            int32_t high = 24 + (i % 2) * 12;

            lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(bar, AI_BAR_BASE_H);

            lv_anim_init(&anim);
            lv_anim_set_var(&anim, bar);
            lv_anim_set_exec_cb(&anim, anim_set_ai_bar_h);
            lv_anim_set_values(&anim, AI_BAR_BASE_H, high);
            lv_anim_set_duration(&anim, 220 + i * 40);
            lv_anim_set_delay(&anim, i * 55);
            lv_anim_set_reverse_duration(&anim, 220 + i * 40);
            lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
            lv_anim_start(&anim);
        }
        else {
            lv_obj_set_height(bar, 18 + (i % 2) * 12);
            set_obj_hidden(bar, true);
        }
    }
    lv_label_set_text(view->answer_label,
                      speaking ? ui_i18n_text(UI_TEXT_AI_LISTENING) : ui_i18n_text(UI_TEXT_AI_IDLE));
}

static void ai_ask_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ui_ai_view_t *view = (ui_ai_view_t *)lv_event_get_user_data(e);

    if(code == LV_EVENT_LONG_PRESSED) {
        ai_set_speaking(view, true);
        if(g_callbacks.ai_question_started != NULL) {
            g_callbacks.ai_question_started();
        }
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ai_set_speaking(view, false);
        if(g_callbacks.ai_question_stopped != NULL) {
            g_callbacks.ai_question_stopped();
        }
    }
}

static void settings_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value;

    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || slider == NULL) {
        return;
    }

    value = lv_slider_get_value(slider);
    if(slider == lv_event_get_user_data(e)) {
        if(g_callbacks.settings_brightness_changed != NULL) {
            g_callbacks.settings_brightness_changed(value);
        }
    }
    else if(g_callbacks.settings_volume_changed != NULL) {
        g_callbacks.settings_volume_changed(value);
    }
}

static void refresh_settings_language(ui_settings_view_t *view)
{
    if(view == NULL) {
        return;
    }

    lv_label_set_text(view->brightness_label, ui_i18n_text(UI_TEXT_SETTINGS_BRIGHTNESS));
    lv_label_set_text(view->volume_label, ui_i18n_text(UI_TEXT_SETTINGS_VOLUME));
    lv_label_set_text(view->language_label, ui_i18n_text(UI_TEXT_SETTINGS_LANGUAGE));
    lv_label_set_text(view->server_label, ui_i18n_text(UI_TEXT_SETTINGS_SERVER));
    lv_label_set_text(view->ip_label, ui_i18n_text(UI_TEXT_SETTINGS_IP));
    lv_label_set_text(view->port_label, ui_i18n_text(UI_TEXT_SETTINGS_PORT));
    lv_dropdown_set_selected(view->language_dropdown, ui_i18n_is_english() ? 1 : 0);
}

static void settings_language_event_cb(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target(e);
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    bool english;

    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || dropdown == NULL) {
        return;
    }

    english = lv_dropdown_get_selected(dropdown) == 1;
    ui_i18n_set_language(english ? UI_LANG_EN : UI_LANG_CN);
    refresh_settings_language(view);
    ui_shell_refresh_language();

    if(g_callbacks.settings_language_changed != NULL) {
        g_callbacks.settings_language_changed(english);
    }
}

static void refresh_language_symbol(ui_settings_view_t *view)
{
    if(view == NULL || view->language_symbol_label == NULL || view->language_dropdown == NULL) {
        return;
    }

    if(lv_dropdown_is_open(view->language_dropdown)) {
        lv_label_set_text(view->language_symbol_label, "^");
        lv_obj_set_pos(view->language_symbol_label, 114, 137);
    }
    else {
        lv_label_set_text(view->language_symbol_label, "v");
        lv_obj_set_pos(view->language_symbol_label, 114, 134);
    }
}

static void settings_language_dropdown_state_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);

    if(lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL) {
        refresh_language_symbol(view);
    }
}

static void settings_language_symbol_event_cb(lv_event_t *e)
{
    lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_user_data(e);

    if(lv_event_get_code(e) != LV_EVENT_CLICKED || dropdown == NULL) {
        return;
    }

    if(lv_dropdown_is_open(dropdown)) {
        lv_dropdown_close(dropdown);
    }
    else {
        lv_dropdown_open(dropdown);
    }
}

void ui_event_set_callbacks(const ui_event_callbacks_t *callbacks)
{
    if(callbacks == NULL) {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
        return;
    }

    g_callbacks = *callbacks;
}

void ui_event_register_intercom(ui_intercom_view_t *view)
{
    if(view == NULL || view->ptt_button == NULL) {
        return;
    }

    refresh_intercom_channel(view);
    set_intercom_talking(view, false);

    lv_obj_add_event_cb(view->channel_dec_button, intercom_dec_event_cb, LV_EVENT_ALL, view);
    lv_obj_add_event_cb(view->channel_inc_button, intercom_inc_event_cb, LV_EVENT_ALL, view);
    lv_obj_add_event_cb(view->ptt_button, intercom_ptt_event_cb, LV_EVENT_ALL, view);
}

void ui_event_register_camera(ui_camera_view_t *view)
{
    if(view == NULL) {
        return;
    }

    lv_obj_add_event_cb(view->capture_button, camera_capture_event_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->upload_button, camera_upload_event_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->retake_button, camera_retake_event_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_state(view->upload_button, LV_STATE_DISABLED);
    lv_obj_add_state(view->retake_button, LV_STATE_DISABLED);
}

void ui_event_register_ai(ui_ai_view_t *view)
{
    if(view == NULL || view->ask_button == NULL) {
        return;
    }

    ai_set_speaking(view, false);
    lv_obj_add_event_cb(view->ask_button, ai_ask_event_cb, LV_EVENT_ALL, view);
}

void ui_event_register_settings(ui_settings_view_t *view)
{
    if(view == NULL) {
        return;
    }

    lv_obj_add_event_cb(view->brightness_slider, settings_slider_event_cb, LV_EVENT_VALUE_CHANGED, view->brightness_slider);
    lv_obj_add_event_cb(view->volume_slider, settings_slider_event_cb, LV_EVENT_VALUE_CHANGED, view->brightness_slider);
    refresh_settings_language(view);
    lv_obj_add_event_cb(view->language_dropdown, settings_language_event_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(view->language_dropdown,
                        settings_language_dropdown_state_event_cb,
                        LV_EVENT_ALL,
                        view);
    lv_obj_add_event_cb(view->language_symbol_label,
                        settings_language_symbol_event_cb,
                        LV_EVENT_CLICKED,
                        view->language_dropdown);
    refresh_language_symbol(view);
}
