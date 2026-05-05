#ifndef UI_EVENT_H
#define UI_EVENT_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Windows simulator:
 *   UI events update visual state directly here. Hardware-side actions can be
 *   tested by registering callbacks from the PC app layer.
 *
 * ESP32-S3 LCD porting:
 *   Keep page creation code unchanged and bind these callbacks to board
 *   drivers/services instead: PTT audio, camera capture/upload, AI recording,
 *   backlight PWM, audio volume, persistent language storage, and server config.
 */
typedef struct {
    void (*intercom_channel_changed)(int32_t channel);
    void (*intercom_ptt_started)(int32_t channel);
    void (*intercom_ptt_stopped)(int32_t channel);
    void (*camera_capture_requested)(void);
    void (*camera_upload_requested)(void);
    void (*camera_retake_requested)(void);
    void (*ai_question_started)(void);
    void (*ai_question_stopped)(void);
    void (*settings_brightness_changed)(int32_t value);
    void (*settings_volume_changed)(int32_t value);
    void (*settings_language_changed)(bool english);
} ui_event_callbacks_t;

typedef struct {
    lv_obj_t *channel_dec_button;
    lv_obj_t *channel_inc_button;
    lv_obj_t *channel_label;
    lv_obj_t *channel_hint_label;
    lv_obj_t *ptt_button;
    lv_obj_t *broadcast_rings[3];
    int32_t channel;
} ui_intercom_view_t;

typedef struct {
    lv_obj_t *capture_label;
    lv_obj_t *upload_label;
    lv_obj_t *retake_label;
    lv_obj_t *preview;
    lv_obj_t *status_label;
    lv_obj_t *capture_button;
    lv_obj_t *upload_button;
    lv_obj_t *retake_button;
    bool frozen;
} ui_camera_view_t;

typedef struct {
    lv_obj_t *answer_label;
    lv_obj_t *ask_button;
    lv_obj_t *ask_label;
    lv_obj_t *voice_bars[4];
    bool speaking;
} ui_ai_view_t;

typedef struct {
    lv_obj_t *brightness_label;
    lv_obj_t *volume_label;
    lv_obj_t *language_label;
    lv_obj_t *language_symbol_label;
    lv_obj_t *server_label;
    lv_obj_t *ip_label;
    lv_obj_t *port_label;
    lv_obj_t *brightness_slider;
    lv_obj_t *volume_slider;
    lv_obj_t *language_dropdown;
} ui_settings_view_t;

void ui_event_set_callbacks(const ui_event_callbacks_t *callbacks);
void ui_event_register_intercom(ui_intercom_view_t *view);
void ui_event_register_camera(ui_camera_view_t *view);
void ui_event_register_ai(ui_ai_view_t *view);
void ui_event_register_settings(ui_settings_view_t *view);

#endif /* UI_EVENT_H */
