/**
 * @file ui_splash.c
 * @brief 可显示真实开机阶段的启动页实现。
 */
#include "ui_splash.h"

#include "app_boot_status.h"
#include "ui.h"
#include "ui_font.h"
#include "ui_shell.h"
#include "lvgl.h"

#include <stdio.h>

#define SPLASH_ROW_COUNT APP_BOOT_STAGE_COUNT
#define SPLASH_ROW_Y     82
#define SPLASH_ROW_H     22

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_rows[SPLASH_ROW_COUNT];
static lv_obj_t *s_error = NULL;

static lv_color_t ui_splash_state_color(app_boot_state_t state)
{
    switch (state) {
    case APP_BOOT_STATE_OK:
        return lv_color_hex(0x41D178);
    case APP_BOOT_STATE_WARN:
        return lv_color_hex(0xF2B84B);
    case APP_BOOT_STATE_ERROR:
        return lv_color_hex(0xF05B5B);
    case APP_BOOT_STATE_RUNNING:
        return lv_color_hex(0xFFFFFF);
    case APP_BOOT_STATE_PENDING:
    default:
        return lv_color_hex(0x777777);
    }
}

static void ui_splash_cleanup(void)
{
    if (s_root != NULL) {
        lv_obj_delete(s_root);
    }

    s_root = NULL;
    s_title = NULL;
    s_error = NULL;
    for (int i = 0; i < SPLASH_ROW_COUNT; i++) {
        s_rows[i] = NULL;
    }
}

void splash_screen(void)
{
    ui_splash_cleanup();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);

    s_root = lv_obj_create(screen);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);

    s_title = lv_label_create(s_root);
    lv_label_set_text(s_title, "Walkie Talkie");
    lv_obj_set_size(s_title, UI_SCREEN_WIDTH, 28);
    lv_obj_set_pos(s_title, 0, 34);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_title, ui_font_normal(), 0);

    for (int i = 0; i < SPLASH_ROW_COUNT; i++) {
        s_rows[i] = lv_label_create(s_root);
        lv_obj_set_size(s_rows[i], UI_SCREEN_WIDTH - 36, SPLASH_ROW_H);
        lv_obj_set_pos(s_rows[i], 18, SPLASH_ROW_Y + (i * SPLASH_ROW_H));
        lv_obj_set_style_text_font(s_rows[i], ui_font_small(), 0);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(0x777777), 0);
    }

    s_error = lv_label_create(s_root);
    lv_obj_set_size(s_error, UI_SCREEN_WIDTH - 28, 36);
    lv_obj_set_pos(s_error, 14, UI_SCREEN_HEIGHT - 52);
    lv_obj_set_style_text_align(s_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_error, ui_font_small(), 0);
    lv_obj_set_style_text_color(s_error, lv_color_hex(0xF05B5B), 0);
    lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);

    ui_splash_refresh_status();
}

void ui_splash_refresh_status(void)
{
    char text[64];

    if (s_root == NULL) {
        return;
    }

    for (int i = 0; i < SPLASH_ROW_COUNT; i++) {
        app_boot_stage_t stage = (app_boot_stage_t)i;
        app_boot_stage_status_t status = app_boot_status_get(stage);

        if (status.code != 0 &&
            (status.state == APP_BOOT_STATE_WARN || status.state == APP_BOOT_STATE_ERROR)) {
            (void)snprintf(text,
                           sizeof(text),
                           "%s  %s (%d)",
                           app_boot_status_stage_name(stage),
                           app_boot_status_state_name(status.state),
                           status.code);
        } else {
            (void)snprintf(text,
                           sizeof(text),
                           "%s  %s",
                           app_boot_status_stage_name(stage),
                           app_boot_status_state_name(status.state));
        }

        lv_label_set_text(s_rows[i], text);
        lv_obj_set_style_text_color(s_rows[i], ui_splash_state_color(status.state), 0);
    }
}

void ui_splash_show_error(const char *stage, int code)
{
    char text[72];

    if (s_error == NULL) {
        return;
    }

    (void)snprintf(text,
                   sizeof(text),
                   "Boot failed: %s (%d)",
                   stage != NULL ? stage : "Unknown",
                   code);
    lv_label_set_text(s_error, text);
    lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);
}

void ui_splash_finish(void)
{
    ui_splash_cleanup();
    ui_shell_init();
}
