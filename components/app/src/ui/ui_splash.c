/**
 * @file ui_splash.c
 * @brief 品牌化启动页实现。
 */
#include "ui_splash.h"

#include "ui.h"
#include "ui_assets.h"
#include "ui_font.h"
#include "ui_shell.h"
#include "lvgl.h"

#define SPLASH_LOGO_Y       70
#define SPLASH_SPINNER_Y    205
#define SPLASH_SPINNER_SIZE 34

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_logo = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_error = NULL;

static void ui_splash_cleanup(void)
{
    if (s_root != NULL) {
        lv_obj_delete(s_root);
    }

    s_root = NULL;
    s_logo = NULL;
    s_spinner = NULL;
    s_error = NULL;
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

    s_logo = lv_image_create(s_root);
    lv_image_set_src(s_logo, &logo);
    lv_obj_set_pos(s_logo, (UI_SCREEN_WIDTH - 96) / 2, SPLASH_LOGO_Y);

    s_spinner = lv_spinner_create(s_root);
    lv_obj_set_size(s_spinner, SPLASH_SPINNER_SIZE, SPLASH_SPINNER_SIZE);
    lv_obj_set_pos(s_spinner,
                   (UI_SCREEN_WIDTH - SPLASH_SPINNER_SIZE) / 2,
                   SPLASH_SPINNER_Y);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);

    s_error = lv_label_create(s_root);
    lv_label_set_text(s_error, "启动失败，请重启");
    lv_obj_set_size(s_error, UI_SCREEN_WIDTH - 28, 36);
    lv_obj_set_pos(s_error, 14, UI_SCREEN_HEIGHT - 52);
    lv_obj_set_style_text_align(s_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_error, ui_font_small(), 0);
    lv_obj_set_style_text_color(s_error, lv_color_hex(0xF05B5B), 0);
    lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
}

void ui_splash_refresh_status(void)
{
    /* 用户启动页不展示内部初始化阶段，状态仍由 app_boot_status 和日志保留。 */
}

void ui_splash_show_error(const char *stage, int code)
{
    (void)stage;
    (void)code;

    if (s_error == NULL) {
        return;
    }

    if (s_spinner != NULL) {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);
}

void ui_splash_finish(void)
{
    ui_splash_cleanup();
    ui_shell_init();
}
