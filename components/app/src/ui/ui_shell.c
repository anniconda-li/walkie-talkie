#include "ui_shell.h"
#include "ui.h"
#include "ui_app_ai.h"
#include "ui_app_camera.h"
#include "ui_app_intercom.h"
#include "ui_app_settings.h"
#include "ui_assets.h"
#include "ui_event.h"
#include "ui_font.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define COLOR_BG               lv_color_hex(0x000000)
#define COLOR_STATUS           lv_color_make(0x22, 0x22, 0x22)
#define COLOR_PANEL            lv_color_make(0x78, 0x78, 0x78)
#define COLOR_BORDER           lv_color_make(0xD0, 0xD0, 0xD0)
#define COLOR_ARROW            lv_color_make(0xF4, 0xF4, 0xF4)
#define STATUS_BAR_H           30
#define STATUS_BAR_Y           0
#define APP_NAME_Y             48

#define BATTERY_BODY_W         26
#define BATTERY_BODY_H         16
#define BATTERY_BORDER_W       2
#define BATTERY_PAD            1
#define BATTERY_FILL_W         (BATTERY_BODY_W - (BATTERY_BORDER_W * 2) - (BATTERY_PAD * 2))
#define BATTERY_FILL_H         (BATTERY_BODY_H - (BATTERY_BORDER_W * 2) - (BATTERY_PAD * 2))

#define MENU_TOGGLE_W          34
#define MENU_TOGGLE_H          40
#define MENU_BAR_W             50
#define MENU_BAR_H             164

#define MENU_BAR_Y             ((UI_SCREEN_HEIGHT - MENU_BAR_H) / 2)
#define MENU_TOGGLE_Y          ((UI_SCREEN_HEIGHT - MENU_TOGGLE_H) / 2)
#define MENU_BAR_OPEN_X        (UI_SCREEN_WIDTH - 3 - MENU_BAR_W)
#define MENU_BAR_HIDDEN_X      UI_SCREEN_WIDTH
#define MENU_TOGGLE_OVERLAP_W  10
#define MENU_TOGGLE_OFFSET_X   (MENU_TOGGLE_OVERLAP_W - MENU_TOGGLE_W)

#define MENU_ICON_SIZE         42
#define MENU_ICON_IMAGE_SCALE  198
#define MENU_ICON_GAP          6
#define MENU_ICON_OFFSET_X     (-1)
#define MENU_APP_COUNT         3
#define MENU_ICON_TOP_PAD      ((MENU_BAR_H - (MENU_APP_COUNT * MENU_ICON_SIZE) - \
                                ((MENU_APP_COUNT - 1) * MENU_ICON_GAP)) / 2)

#define ENTRY_FROM_TOP(y)      ((y) - STATUS_BAR_H - 10)
#define ENTRY_FROM_RIGHT(x)    (UI_SCREEN_WIDTH + 10)

typedef struct {
    lv_obj_t * selector;
    lv_obj_t * box;
    lv_obj_t * icon;
} menu_icon_t;

static lv_obj_t *g_bg;
static lv_obj_t *g_status_bar;
static lv_obj_t *g_app_name_label;
static lv_obj_t *g_signal_bars[4];
static lv_obj_t *g_wifi_arcs[3];
static lv_obj_t *g_wifi_dot;
static lv_obj_t *g_battery_level;
static lv_obj_t *g_battery_label;
static uint8_t g_battery_percent = 60;
static uint8_t g_wifi_signal_level = 3;
static uint8_t g_cellular_signal_level = 0;
static lv_obj_t *g_app_content_root;
static lv_obj_t *g_current_app_root;
static lv_obj_t *g_menu_toggle;
static lv_obj_t *g_menu_bar;
static lv_obj_t *g_menu_clip;
static lv_obj_t *g_menu_toggle_arrow;
static lv_obj_t *g_power_dialog;
static menu_icon_t g_menu_icons[UI_APP_ID_COUNT];

static ui_app_id_t g_current_app = UI_APP_ID_INTERCOM;
static ui_app_id_t g_center_app = UI_APP_ID_INTERCOM;
static bool g_menu_expanded;
static bool g_switch_requested;

static const ui_text_id_t g_app_text_ids[UI_APP_ID_COUNT] = {
    UI_TEXT_APP_INTERCOM,
    UI_TEXT_APP_CAMERA,
    UI_TEXT_APP_AI,
    UI_TEXT_APP_SETTINGS
};

static const ui_app_id_t g_menu_apps[MENU_APP_COUNT] = {
    UI_APP_ID_INTERCOM,
    UI_APP_ID_AI,
    UI_APP_ID_SETTINGS
};

static void anim_set_x(void *obj, int32_t x);
static void update_menu_icons(void);
static void power_dialog_close_event_cb(lv_event_t *e);
static void power_dialog_confirm_event_cb(lv_event_t *e);

static const lv_image_dsc_t *get_app_icon_src(ui_app_id_t app)
{
    switch(app) {
        case UI_APP_ID_INTERCOM:
            return &intercom;
        case UI_APP_ID_AI:
            return &ai;
        case UI_APP_ID_SETTINGS:
            return &settings;
        default:
            return NULL;
    }
}

static void refresh_menu_toggle_arrow(void)
{
    if(g_menu_toggle_arrow == NULL) {
        return;
    }

    lv_label_set_text(g_menu_toggle_arrow, g_menu_expanded ? ">" : "<");
    lv_obj_center(g_menu_toggle_arrow);
}

static void close_power_dialog(void)
{
    if(g_power_dialog == NULL) {
        return;
    }

    lv_obj_delete(g_power_dialog);
    g_power_dialog = NULL;
}

static void power_dialog_close_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        close_power_dialog();
    }
}

static void power_dialog_confirm_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    close_power_dialog();
    ui_event_notify_power_shutdown_confirmed();
}

static void apply_menu_icon_visual(lv_obj_t *target_box,
                                   lv_obj_t *target_selector,
                                   lv_obj_t *target_icon,
                                   ui_app_id_t app,
                                   int32_t size,
                                   int32_t x,
                                   int32_t y,
                                   bool highlighted)
{
    lv_obj_remove_flag(target_selector, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(target_selector, size + 4, size + 4);
    lv_obj_set_pos(target_selector, x - 2, y - 2);
    lv_obj_set_style_radius(target_selector, 10, 0);
    lv_obj_set_style_bg_color(target_selector, COLOR_ARROW, 0);
    lv_obj_set_style_bg_opa(target_selector, highlighted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(target_selector, 0, 0);
    lv_obj_set_style_pad_all(target_selector, 0, 0);

    lv_obj_set_size(target_box, size, size);
    lv_obj_set_pos(target_box, x, y);
    lv_obj_set_style_radius(target_box, 8, 0);
    lv_obj_set_style_clip_corner(target_box, true, 0);
    lv_obj_set_style_bg_color(target_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(target_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(target_box, 0, 0);

    lv_image_set_src(target_icon, get_app_icon_src(app));
    lv_image_set_scale(target_icon, MENU_ICON_IMAGE_SCALE);
    lv_obj_center(target_icon);
}

static lv_obj_t *create_base_rect(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void anim_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void anim_set_y(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

static void refresh_app_title(ui_app_id_t app)
{
    lv_label_set_text(g_app_name_label, ui_i18n_text(g_app_text_ids[app]));
    lv_obj_set_style_text_color(g_app_name_label, ui_theme_app_color(app), 0);
}

static void refresh_menu_visibility(void)
{
    bool hide = g_current_app == UI_APP_ID_CAMERA;

    if(g_menu_toggle != NULL) {
        if(hide) {
            lv_obj_add_flag(g_menu_toggle, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_remove_flag(g_menu_toggle, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(g_menu_bar != NULL) {
        if(hide) {
            lv_obj_add_flag(g_menu_bar, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_remove_flag(g_menu_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_menu_position(int32_t menu_x)
{
    lv_anim_del(g_menu_toggle, anim_set_x);
    lv_anim_del(g_menu_bar, anim_set_x);
    lv_obj_set_x(g_menu_bar, menu_x);
    lv_obj_set_x(g_menu_toggle, menu_x + MENU_TOGGLE_OFFSET_X);
}

static void collapse_menu_now(void)
{
    if(!g_menu_expanded) {
        return;
    }

    g_menu_expanded = false;
    refresh_menu_toggle_arrow();
    set_menu_position(MENU_BAR_HIDDEN_X);
}

static void create_status_signal(lv_obj_t *parent)
{
    const int32_t base_y = 23;

    for(int32_t i = 0; i < 4; i++) {
        int32_t h = 6 + i * 3;

        g_signal_bars[i] = lv_obj_create(parent);
        lv_obj_remove_flag(g_signal_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(g_signal_bars[i], 4, h);
        lv_obj_set_pos(g_signal_bars[i], 9 + i * 6, base_y - h);
        lv_obj_set_style_radius(g_signal_bars[i], 1, 0);
        lv_obj_set_style_bg_color(g_signal_bars[i], COLOR_ARROW, 0);
        lv_obj_set_style_bg_opa(g_signal_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_signal_bars[i], 0, 0);
        lv_obj_set_style_pad_all(g_signal_bars[i], 0, 0);
    }

    const int32_t wifi_center_x = 54;
    const int32_t wifi_center_y = 23;
    const int32_t wifi_arc_sizes[3] = {12, 22, 32};

    for(int32_t i = 0; i < 3; i++) {
        int32_t size = wifi_arc_sizes[i];
        int32_t x = wifi_center_x - (size / 2);
        int32_t y = wifi_center_y - (size / 2);

        g_wifi_arcs[i] = lv_arc_create(parent);
        lv_obj_remove_flag(g_wifi_arcs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(g_wifi_arcs[i], size, size);
        lv_obj_set_pos(g_wifi_arcs[i], x, y);
        lv_arc_set_bg_angles(g_wifi_arcs[i], 225, 315);
        lv_obj_set_style_arc_width(g_wifi_arcs[i], 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_wifi_arcs[i], COLOR_ARROW, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_wifi_arcs[i], 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(g_wifi_arcs[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_wifi_arcs[i], 0, 0);
        lv_obj_remove_style(g_wifi_arcs[i], NULL, LV_PART_KNOB);
    }

    g_wifi_dot = lv_obj_create(parent);
    lv_obj_remove_flag(g_wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_wifi_dot, 4, 4);
    lv_obj_set_pos(g_wifi_dot, wifi_center_x - 2, wifi_center_y - 2);
    lv_obj_set_style_radius(g_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_wifi_dot, COLOR_ARROW, 0);
    lv_obj_set_style_bg_opa(g_wifi_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_wifi_dot, 0, 0);
    lv_obj_set_style_pad_all(g_wifi_dot, 0, 0);
}

static void refresh_signal_level(void)
{
    for(int32_t i = 0; i < 4; i++) {
        if(g_signal_bars[i] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_opa(g_signal_bars[i],
                                i < g_cellular_signal_level ? LV_OPA_COVER : LV_OPA_30,
                                0);
    }

    for(int32_t i = 0; i < 3; i++) {
        if(g_wifi_arcs[i] == NULL) {
            continue;
        }
        lv_obj_set_style_arc_opa(g_wifi_arcs[i],
                                 (i + 2) <= g_wifi_signal_level ? LV_OPA_COVER : LV_OPA_30,
                                 LV_PART_MAIN);
    }

    if(g_wifi_dot != NULL) {
        lv_obj_set_style_bg_opa(g_wifi_dot, g_wifi_signal_level > 0 ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void create_status_battery(lv_obj_t *parent)
{
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(body, BATTERY_BODY_W, BATTERY_BODY_H);
    lv_obj_set_pos(body, UI_SCREEN_WIDTH - 68, 7);
    lv_obj_set_style_radius(body, 4, 0);
    lv_obj_set_style_clip_corner(body, true, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, COLOR_ARROW, 0);
    lv_obj_set_style_border_width(body, BATTERY_BORDER_W, 0);
    lv_obj_set_style_pad_all(body, BATTERY_PAD, 0);

    g_battery_level = lv_obj_create(body);
    lv_obj_remove_flag(g_battery_level, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_battery_level, BATTERY_FILL_W, BATTERY_FILL_H);
    lv_obj_set_pos(g_battery_level, 0, 0);
    lv_obj_set_style_radius(g_battery_level, 1, 0);
    lv_obj_set_style_bg_color(g_battery_level, lv_color_make(0x41, 0xD1, 0x78), 0);
    lv_obj_set_style_bg_opa(g_battery_level, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_battery_level, 0, 0);
    lv_obj_set_style_pad_all(g_battery_level, 0, 0);

    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cap, 4, 9);
    lv_obj_set_pos(cap, UI_SCREEN_WIDTH - 42, 10);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_bg_color(cap, COLOR_ARROW, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_pad_all(cap, 0, 0);

    g_battery_label = lv_label_create(parent);
    lv_obj_set_size(g_battery_label, 34, 16);
    lv_obj_set_pos(g_battery_label, UI_SCREEN_WIDTH - 36, 5);
    lv_obj_set_style_text_color(g_battery_label, COLOR_ARROW, 0);
    lv_obj_set_style_text_font(g_battery_label, ui_font_small(), 0);
    lv_obj_set_style_text_align(g_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
}

static void refresh_battery_level(void)
{
    int32_t width;
    char text[8];

    if(g_battery_level == NULL) {
        return;
    }

    width = (BATTERY_FILL_W * (int32_t)g_battery_percent) / 100;
    lv_obj_set_width(g_battery_level, width);

    if(g_battery_label != NULL) {
        lv_snprintf(text, sizeof(text), "%u%%", (unsigned int)g_battery_percent);
        lv_label_set_text(g_battery_label, text);
    }
}

static void update_menu_icons(void)
{
    for(int32_t i = 0; i < MENU_APP_COUNT; i++) {
        ui_app_id_t app = g_menu_apps[i];
        int32_t y = MENU_ICON_TOP_PAD + i * (MENU_ICON_SIZE + MENU_ICON_GAP);
        int32_t x = ((MENU_BAR_W - MENU_ICON_SIZE) / 2) + MENU_ICON_OFFSET_X;
        bool highlighted = app == g_center_app;

        apply_menu_icon_visual(g_menu_icons[i].box,
                               g_menu_icons[i].selector,
                               g_menu_icons[i].icon,
                               app, MENU_ICON_SIZE, x, y, highlighted);

        if(highlighted) {
            lv_obj_move_foreground(g_menu_icons[i].selector);
            lv_obj_move_foreground(g_menu_icons[i].box);
        }
    }

    refresh_app_title(g_center_app);
}

static lv_obj_t *create_app_root(ui_app_id_t app)
{
    switch(app) {
        case UI_APP_ID_INTERCOM:
            return ui_app_intercom_create(g_app_content_root);
        case UI_APP_ID_CAMERA:
            return ui_app_camera_create(g_app_content_root);
        case UI_APP_ID_AI:
            return ui_app_ai_create(g_app_content_root);
        case UI_APP_ID_SETTINGS:
            return ui_app_settings_create(g_app_content_root);
        default:
            return NULL;
    }
}

static void enter_app(ui_app_id_t app, lv_obj_t * root)
{
    switch(app) {
        case UI_APP_ID_INTERCOM:
            ui_app_intercom_enter(root);
            break;
        case UI_APP_ID_CAMERA:
            ui_app_camera_enter(root);
            break;
        case UI_APP_ID_AI:
            ui_app_ai_enter(root);
            break;
        case UI_APP_ID_SETTINGS:
            ui_app_settings_enter(root);
            break;
        default:
            break;
    }
}

static void app_switch_done_cb(lv_anim_t *a)
{
    (void)a;

    /* 旧页面直接删除后创建新页面，避免退场和入场动画串行拉长切页时间。 */
    g_current_app_root = create_app_root(g_center_app);
    g_current_app = g_center_app;
    g_switch_requested = false;

    ui_event_notify_app_changed((int32_t)g_current_app);
    if(g_current_app_root) {
        enter_app(g_current_app, g_current_app_root);
    }
    refresh_menu_visibility();
}

static void request_app_switch_if_needed(void)
{
    if(g_switch_requested || g_current_app == g_center_app) {
        return;
    }

    g_switch_requested = true;

    switch(g_current_app) {
        case UI_APP_ID_INTERCOM:
            ui_app_intercom_exit(g_current_app_root, app_switch_done_cb);
            break;
        case UI_APP_ID_CAMERA:
            ui_app_camera_exit(g_current_app_root, app_switch_done_cb);
            break;
        case UI_APP_ID_AI:
            ui_app_ai_exit(g_current_app_root, app_switch_done_cb);
            break;
        case UI_APP_ID_SETTINGS:
            ui_app_settings_exit(g_current_app_root, app_switch_done_cb);
            break;
        default:
            break;
    }
}

static void start_menu_motion(int32_t menu_x)
{
    int32_t toggle_x = menu_x + MENU_TOGGLE_OFFSET_X;

    lv_anim_del(g_menu_toggle, anim_set_x);
    lv_anim_del(g_menu_bar, anim_set_x);

    lv_anim_t toggle_anim;
    lv_anim_init(&toggle_anim);
    lv_anim_set_var(&toggle_anim, g_menu_toggle);
    lv_anim_set_exec_cb(&toggle_anim, anim_set_x);
    lv_anim_set_values(&toggle_anim, lv_obj_get_x(g_menu_toggle), toggle_x);
    lv_anim_set_duration(&toggle_anim, 180);
    lv_anim_set_path_cb(&toggle_anim, lv_anim_path_ease_out);
    lv_anim_start(&toggle_anim);

    lv_anim_t menu_anim;
    lv_anim_init(&menu_anim);
    lv_anim_set_var(&menu_anim, g_menu_bar);
    lv_anim_set_exec_cb(&menu_anim, anim_set_x);
    lv_anim_set_values(&menu_anim, lv_obj_get_x(g_menu_bar), menu_x);
    lv_anim_set_duration(&menu_anim, 180);
    lv_anim_set_path_cb(&menu_anim, lv_anim_path_ease_out);
    lv_anim_start(&menu_anim);
}

static void menu_toggle_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || g_switch_requested) {
        return;
    }

    g_menu_expanded = !g_menu_expanded;
    refresh_menu_toggle_arrow();
    start_menu_motion(g_menu_expanded ? MENU_BAR_OPEN_X : MENU_BAR_HIDDEN_X);
}

static void menu_icon_event_cb(lv_event_t *e)
{
    ui_app_id_t app = (ui_app_id_t)(intptr_t)lv_event_get_user_data(e);

    if(lv_event_get_code(e) != LV_EVENT_CLICKED || g_switch_requested) {
        return;
    }

    g_center_app = app;
    update_menu_icons();
    collapse_menu_now();
    request_app_switch_if_needed();
}

static void menu_entry_done_cb(lv_anim_t *a)
{
    (void)a;
    start_menu_motion(MENU_BAR_HIDDEN_X);
}

static void status_entry_done_cb(lv_anim_t *a)
{
    (void)a;

    lv_anim_t toggle_anim;
    lv_anim_init(&toggle_anim);
    lv_anim_set_var(&toggle_anim, g_menu_toggle);
    lv_anim_set_exec_cb(&toggle_anim, anim_set_x);
    lv_anim_set_values(&toggle_anim,
                       ENTRY_FROM_RIGHT(MENU_BAR_HIDDEN_X + MENU_TOGGLE_OFFSET_X),
                       MENU_BAR_HIDDEN_X + MENU_TOGGLE_OFFSET_X);
    lv_anim_set_duration(&toggle_anim, 180);
    lv_anim_set_path_cb(&toggle_anim, lv_anim_path_ease_out);
    lv_anim_start(&toggle_anim);

    lv_anim_t menu_anim;
    lv_anim_init(&menu_anim);
    lv_anim_set_var(&menu_anim, g_menu_bar);
    lv_anim_set_exec_cb(&menu_anim, anim_set_x);
    lv_anim_set_values(&menu_anim, ENTRY_FROM_RIGHT(MENU_BAR_HIDDEN_X), MENU_BAR_HIDDEN_X);
    lv_anim_set_duration(&menu_anim, 180);
    lv_anim_set_path_cb(&menu_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&menu_anim, menu_entry_done_cb);
    lv_anim_start(&menu_anim);
}

static void start_entry_animation(void)
{
    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_status_bar);
    lv_anim_set_exec_cb(&anim, anim_set_y);
    lv_anim_set_values(&anim, ENTRY_FROM_TOP(STATUS_BAR_Y), STATUS_BAR_Y);
    lv_anim_set_duration(&anim, 260);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, status_entry_done_cb);
    lv_anim_start(&anim);
}

static void create_menu_icons(void)
{
    for(int32_t i = 0; i < MENU_APP_COUNT; i++) {
        g_menu_icons[i].selector = lv_obj_create(g_menu_clip);
        lv_obj_remove_flag(g_menu_icons[i].selector, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(g_menu_icons[i].selector, 0, 0);
        lv_obj_set_style_pad_all(g_menu_icons[i].selector, 0, 0);
        lv_obj_remove_flag(g_menu_icons[i].selector, LV_OBJ_FLAG_CLICKABLE);

        g_menu_icons[i].box = lv_obj_create(g_menu_clip);
        lv_obj_remove_flag(g_menu_icons[i].box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(g_menu_icons[i].box, 0, 0);
        lv_obj_set_style_pad_all(g_menu_icons[i].box, 0, 0);
        lv_obj_add_flag(g_menu_icons[i].box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_menu_icons[i].box,
                            menu_icon_event_cb,
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)g_menu_apps[i]);

        g_menu_icons[i].icon = lv_image_create(g_menu_icons[i].box);
        lv_obj_remove_flag(g_menu_icons[i].icon, LV_OBJ_FLAG_CLICKABLE);
    }
}

void ui_shell_init(void)
{
    g_bg = lv_screen_active();
    lv_obj_set_size(g_bg, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(g_bg, COLOR_BG, 0);
    lv_obj_remove_flag(g_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_bg, LV_SCROLLBAR_MODE_OFF);

    g_status_bar = lv_obj_create(g_bg);
    lv_obj_remove_flag(g_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_status_bar, UI_SCREEN_WIDTH, STATUS_BAR_H);
    lv_obj_set_pos(g_status_bar, 0, ENTRY_FROM_TOP(STATUS_BAR_Y));
    lv_obj_set_style_bg_color(g_status_bar, COLOR_STATUS, 0);
    lv_obj_set_style_border_width(g_status_bar, 0, 0);
    lv_obj_set_style_radius(g_status_bar, 0, 0);
    lv_obj_set_style_pad_all(g_status_bar, 0, 0);

    create_status_signal(g_status_bar);
    create_status_battery(g_status_bar);
    refresh_signal_level();
    refresh_battery_level();

    g_app_name_label = lv_label_create(g_status_bar);
    lv_obj_set_size(g_app_name_label, 112, 18);
    lv_obj_set_pos(g_app_name_label, 64, 6);
    lv_obj_set_style_text_align(g_app_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_app_name_label, COLOR_ARROW, 0);
    lv_obj_set_style_text_font(g_app_name_label, ui_font_small(), 0);
    refresh_app_title(UI_APP_ID_INTERCOM);

    g_app_content_root = lv_obj_create(g_bg);
    lv_obj_remove_flag(g_app_content_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_app_content_root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(g_app_content_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_app_content_root, 0, 0);
    lv_obj_set_style_radius(g_app_content_root, 0, 0);
    lv_obj_set_style_pad_all(g_app_content_root, 0, 0);

    g_menu_toggle = create_base_rect(g_bg, MENU_TOGGLE_W, MENU_TOGGLE_H);
    lv_obj_set_style_radius(g_menu_toggle, 5, 0);
    lv_obj_set_style_bg_opa(g_menu_toggle, LV_OPA_70, 0);
    lv_obj_set_pos(g_menu_toggle, ENTRY_FROM_RIGHT(MENU_BAR_HIDDEN_X + MENU_TOGGLE_OFFSET_X), MENU_TOGGLE_Y);
    lv_obj_add_flag(g_menu_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_menu_toggle, menu_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    g_menu_toggle_arrow = lv_label_create(g_menu_toggle);
    lv_obj_set_style_text_color(g_menu_toggle_arrow, COLOR_ARROW, 0);
    lv_obj_center(g_menu_toggle_arrow);

    g_menu_bar = create_base_rect(g_bg, MENU_BAR_W, MENU_BAR_H);
    lv_obj_set_style_radius(g_menu_bar, 5, 0);
    lv_obj_set_pos(g_menu_bar, ENTRY_FROM_RIGHT(MENU_BAR_HIDDEN_X), MENU_BAR_Y);
    lv_obj_set_style_pad_all(g_menu_bar, 0, 0);

    g_menu_clip = lv_obj_create(g_menu_bar);
    lv_obj_remove_flag(g_menu_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_menu_clip, MENU_BAR_W, MENU_BAR_H);
    lv_obj_set_pos(g_menu_clip, 0, 0);
    lv_obj_set_style_bg_opa(g_menu_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_menu_clip, 0, 0);
    lv_obj_set_style_pad_all(g_menu_clip, 0, 0);
    lv_obj_set_style_radius(g_menu_clip, 20, 0);

    create_menu_icons();

    g_current_app = UI_APP_ID_INTERCOM;
    g_center_app = UI_APP_ID_INTERCOM;
    g_current_app_root = create_app_root(g_current_app);
    if(g_current_app_root) {
        enter_app(g_current_app, g_current_app_root);
    }
    ui_event_notify_app_changed((int32_t)g_current_app);

    g_menu_expanded = false;
    g_switch_requested = false;

    refresh_menu_toggle_arrow();
    update_menu_icons();
    refresh_menu_visibility();
    lv_obj_move_foreground(g_status_bar);
    lv_obj_move_foreground(g_menu_toggle);
    lv_obj_move_foreground(g_menu_bar);
    start_entry_animation();
}

void ui_shell_refresh_language(void)
{
    refresh_app_title(g_center_app);
}

void ui_shell_switch_to(ui_app_id_t app)
{
    if(app < UI_APP_ID_INTERCOM || app >= UI_APP_ID_COUNT || g_switch_requested) {
        return;
    }

    g_center_app = app;
    update_menu_icons();
    collapse_menu_now();
    request_app_switch_if_needed();
}

void ui_shell_set_battery_level(uint8_t percent)
{
    if(percent > 100) {
        percent = 100;
    }

    g_battery_percent = percent;
    refresh_battery_level();
}

void ui_shell_set_signal_level(uint8_t level)
{
    ui_shell_set_signal_levels(level, 0);
}

void ui_shell_set_signal_levels(uint8_t wifi_level, uint8_t cellular_level)
{
    if(wifi_level > 4) {
        wifi_level = 4;
    }
    if(cellular_level > 4) {
        cellular_level = 4;
    }

    g_wifi_signal_level = wifi_level;
    g_cellular_signal_level = cellular_level;
    refresh_signal_level();
}

void ui_shell_show_power_dialog(void)
{
    if(g_bg == NULL) {
        return;
    }

    if(g_power_dialog != NULL) {
        lv_obj_move_foreground(g_power_dialog);
        return;
    }

    g_power_dialog = lv_obj_create(g_bg);
    lv_obj_remove_flag(g_power_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_power_dialog, 124, 112);
    lv_obj_align(g_power_dialog, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(g_power_dialog, 18, 0);
    lv_obj_set_style_bg_color(g_power_dialog, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_bg_opa(g_power_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_power_dialog, lv_color_make(0x58, 0x58, 0x58), 0);
    lv_obj_set_style_border_width(g_power_dialog, 1, 0);
    lv_obj_set_style_pad_all(g_power_dialog, 0, 0);

    lv_obj_t *close_btn = lv_button_create(g_power_dialog);
    lv_obj_set_size(close_btn, 28, 28);
    lv_obj_set_pos(close_btn, 88, 8);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_make(0x32, 0x32, 0x32), 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, power_dialog_close_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_obj_center(close_label);

    lv_obj_t *power_btn = lv_button_create(g_power_dialog);
    lv_obj_set_size(power_btn, 64, 64);
    lv_obj_align(power_btn, LV_ALIGN_CENTER, 0, 7);
    lv_obj_set_style_radius(power_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(power_btn, lv_color_make(0xE8, 0xE8, 0xE8), 0);
    lv_obj_set_style_shadow_width(power_btn, 0, 0);
    lv_obj_add_event_cb(power_btn, power_dialog_confirm_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *power_label = lv_label_create(power_btn);
    lv_label_set_text(power_label, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(power_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(power_label, ui_font_normal(), 0);
    lv_obj_center(power_label);

    lv_obj_move_foreground(g_power_dialog);
}
