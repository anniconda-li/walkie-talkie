/**
 * @file ui_app_settings.c
 * @brief 设置页面 UI——网络、固件版本信息。
 *
 * ## 页面布局（240×280 屏幕）
 * ```
 * ┌──────────────────────────┐
 * │    状态栏（shell 管理）    │  y=0..30
 * ├──────────────────────────┤
 * │  ┌────────────────────┐  │
 * │  │ WLAN   移动数据     │  │
 * │  │                    │  │
 * │  │ 固件版本            │  │
 * │  │ v1.0.0              │  │
 * │  └────────────────────┘  │
 * └──────────────────────────┘
 * ```
 *
 * ## 交互
 * - 网络按钮：LV_EVENT_CLICKED → 回调到 app_business
 *
 * ## 动画说明
 * - 入场：面板整体短距离滑入
 * - 退场：停止页面动画后直接删除 root → 调用 done_cb
 */
#include "ui_app_settings.h"
#include "ui.h"
#include "ui_assets.h"
#include "ui_event.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "esp_app_desc.h"

/** @brief 设置页面全局视图对象。 */
static ui_settings_view_t g_settings_view;

/** @brief 设置面板（深灰圆角矩形）。 */
static lv_obj_t *g_settings_panel;

/** @brief LVGL 动画回调：设置对象 y 坐标。 */
static void anim_set_y(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

/** @brief 启动 Y 轴位移动画。 */
static void start_y_anim(lv_obj_t *obj,
                         int32_t from,
                         int32_t to,
                         uint32_t duration,
                         uint32_t delay,
                         lv_anim_path_cb_t path_cb,
                         lv_anim_completed_cb_t done_cb,
                         void *user_data)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, anim_set_y);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, path_cb);
    if(done_cb != NULL) {
        lv_anim_set_user_data(&anim, user_data);
        lv_anim_set_completed_cb(&anim, done_cb);
    }
    lv_anim_start(&anim);
}

/**
 * @brief 创建设置面板（深灰圆角矩形容器）。
 *
 * @param parent 父容器。
 * @return 面板 LVGL 对象。
 */
static lv_obj_t *create_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, 14, 48);
    lv_obj_set_size(panel, 212, 252);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x24, 0x24, 0x24), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    return panel;
}

/**
 * @brief 创建标签文字（浅灰色标题）。
 *
 * @param parent 父容器。
 * @param text   显示文本。
 * @param y      垂直位置。
 * @return 标签 LVGL 对象。
 */
static lv_obj_t *create_caption(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, 12, y);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

static lv_obj_t *create_setting_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    return box;
}

static lv_obj_t *create_network_button(lv_obj_t *parent,
                                       const char *text,
                                       int32_t x,
                                       int32_t y,
                                       lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 90, 58);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_size(label, 78, 24);
    lv_obj_center(label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    if(label_out != NULL) {
        *label_out = label;
    }
    return btn;
}

static lv_obj_t *create_small_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 30);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SETTINGS, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return btn;
}

static lv_obj_t *create_wlan_page(lv_obj_t *root)
{
    lv_obj_t *page = lv_obj_create(root);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    g_settings_view.wlan_back_button = lv_button_create(page);
    lv_obj_set_pos(g_settings_view.wlan_back_button, 8, 34);
    lv_obj_set_size(g_settings_view.wlan_back_button, 68, 34);
    lv_obj_set_style_radius(g_settings_view.wlan_back_button, 8, 0);
    lv_obj_set_style_bg_color(g_settings_view.wlan_back_button, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_bg_opa(g_settings_view.wlan_back_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_settings_view.wlan_back_button, lv_color_make(0x75, 0x75, 0x75), 0);
    lv_obj_set_style_border_width(g_settings_view.wlan_back_button, 1, 0);
    lv_obj_set_style_shadow_width(g_settings_view.wlan_back_button, 0, 0);

    lv_obj_t *back_icon = lv_image_create(g_settings_view.wlan_back_button);
    lv_image_set_src(back_icon, &icon_camera_back);
    lv_image_set_scale(back_icon, 160);
    lv_obj_center(back_icon);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "WLAN");
    lv_obj_set_pos(title, 88, 42);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    g_settings_view.wlan_scan_button = create_small_button(page, "扫描", 168, 36, 58);

    g_settings_view.wlan_status_label = lv_label_create(page);
    lv_label_set_text(g_settings_view.wlan_status_label, "长按 WLAN 进入扫描");
    lv_obj_set_pos(g_settings_view.wlan_status_label, 16, 74);
    lv_obj_set_width(g_settings_view.wlan_status_label, 208);
    lv_obj_set_style_text_color(g_settings_view.wlan_status_label, lv_color_make(0xD8, 0xD8, 0xD8), 0);

    g_settings_view.wlan_list = lv_obj_create(page);
    lv_obj_set_pos(g_settings_view.wlan_list, 16, 96);
    lv_obj_set_size(g_settings_view.wlan_list, 208, 160);
    lv_obj_set_style_bg_opa(g_settings_view.wlan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_settings_view.wlan_list, 0, 0);
    lv_obj_set_style_pad_all(g_settings_view.wlan_list, 0, 0);
    lv_obj_set_style_pad_row(g_settings_view.wlan_list, 6, 0);
    lv_obj_set_flex_flow(g_settings_view.wlan_list, LV_FLEX_FLOW_COLUMN);

    g_settings_view.password_dialog = lv_obj_create(page);
    lv_obj_remove_flag(g_settings_view.password_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_settings_view.password_dialog, 16, 36);
    lv_obj_set_size(g_settings_view.password_dialog, 208, 126);
    lv_obj_set_style_radius(g_settings_view.password_dialog, 8, 0);
    lv_obj_set_style_bg_color(g_settings_view.password_dialog, lv_color_make(0x24, 0x24, 0x24), 0);
    lv_obj_set_style_bg_opa(g_settings_view.password_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_settings_view.password_dialog, UI_COLOR_SETTINGS, 0);
    lv_obj_set_style_border_width(g_settings_view.password_dialog, 2, 0);
    lv_obj_set_style_pad_all(g_settings_view.password_dialog, 10, 0);
    lv_obj_add_flag(g_settings_view.password_dialog, LV_OBJ_FLAG_HIDDEN);

    g_settings_view.password_title_label = lv_label_create(g_settings_view.password_dialog);
    lv_label_set_text(g_settings_view.password_title_label, "输入密码");
    lv_obj_set_pos(g_settings_view.password_title_label, 0, 0);
    lv_obj_set_width(g_settings_view.password_title_label, 188);
    lv_label_set_long_mode(g_settings_view.password_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(g_settings_view.password_title_label, lv_color_white(), 0);

    g_settings_view.password_textarea = lv_textarea_create(g_settings_view.password_dialog);
    lv_obj_set_pos(g_settings_view.password_textarea, 0, 30);
    lv_obj_set_size(g_settings_view.password_textarea, 188, 34);
    lv_textarea_set_password_mode(g_settings_view.password_textarea, true);
    lv_textarea_set_one_line(g_settings_view.password_textarea, true);
    lv_textarea_set_placeholder_text(g_settings_view.password_textarea, "密码");

    g_settings_view.cancel_button = create_small_button(g_settings_view.password_dialog, "取消", 0, 76, 86);
    g_settings_view.connect_button = create_small_button(g_settings_view.password_dialog, "连接", 102, 76, 86);

    g_settings_view.connecting_spinner = lv_spinner_create(g_settings_view.password_dialog);
    lv_obj_set_size(g_settings_view.connecting_spinner, 34, 34);
    lv_obj_align(g_settings_view.connecting_spinner, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_arc_color(g_settings_view.connecting_spinner, lv_color_make(0x42, 0x42, 0x42), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_settings_view.connecting_spinner, UI_COLOR_SETTINGS, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_settings_view.connecting_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_settings_view.connecting_spinner, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(g_settings_view.connecting_spinner, LV_OBJ_FLAG_HIDDEN);

    g_settings_view.password_keyboard = lv_keyboard_create(page);
    lv_obj_set_size(g_settings_view.password_keyboard, 240, 112);
    lv_obj_align(g_settings_view.password_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(g_settings_view.password_keyboard, g_settings_view.password_textarea);
    lv_obj_add_flag(g_settings_view.password_keyboard, LV_OBJ_FLAG_HIDDEN);

    return page;
}

/**
 * @brief 设置固件版本标签。
 *
 * 版本号来自 ESP-IDF 应用描述，和最终固件镜像中的版本字段保持一致。
 */
static void set_firmware_version_label(lv_obj_t *label)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = (app_desc != NULL && app_desc->version[0] != '\0') ? app_desc->version : "unknown";
    char text[64];

    if (version[0] == 'v' || version[0] == 'V') {
        lv_snprintf(text, sizeof(text), "%s", version);
    } else {
        lv_snprintf(text, sizeof(text), "v%s", version);
    }
    lv_label_set_text(label, text);
}

/**
 * @brief 创建设置页面完整 UI。
 *
 * 控件创建顺序与视觉布局一致（从上到下）。
 *
 * @param parent 挂载的父容器（g_app_content_root）。
 * @return 设置页面根对象。
 */
lv_obj_t * ui_app_settings_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    g_settings_panel = create_panel(root);

    g_settings_view.wlan_button = create_network_button(g_settings_panel,
                                                        "WLAN",
                                                        2,
                                                        2,
                                                        &g_settings_view.wlan_ssid_label);

    g_settings_view.cellular_button = create_network_button(g_settings_panel,
                                                            "移动数据",
                                                            100,
                                                            2,
                                                            &g_settings_view.cellular_label);

    lv_obj_t *firmware_box = create_setting_box(g_settings_panel, 2, 70, 188, 58);

    /* 固件版本信息行（只读标签） */
    g_settings_view.firmware_label = create_caption(firmware_box, ui_i18n_text(UI_TEXT_SETTINGS_FIRMWARE), 8);

    g_settings_view.version_label = lv_label_create(firmware_box);
    set_firmware_version_label(g_settings_view.version_label);
    lv_obj_set_pos(g_settings_view.version_label, 12, 33);
    lv_obj_set_width(g_settings_view.version_label, 164);
    lv_label_set_long_mode(g_settings_view.version_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(g_settings_view.version_label, lv_color_white(), 0);

    g_settings_view.network_status_label = lv_label_create(g_settings_panel);
    lv_label_set_text(g_settings_view.network_status_label, "");
    lv_obj_set_pos(g_settings_view.network_status_label, 2, 210);
    lv_obj_set_width(g_settings_view.network_status_label, 188);
    lv_obj_set_style_text_color(g_settings_view.network_status_label, lv_color_make(0xD8, 0xD8, 0xD8), 0);
    lv_obj_add_flag(g_settings_view.network_status_label, LV_OBJ_FLAG_HIDDEN);

    g_settings_view.wlan_page = create_wlan_page(root);
    ui_event_register_settings(&g_settings_view);
    return root;
}

/**
 * @brief 播放设置页面入场动画。
 *
 * 面板从上方(-260)滑入到 y=48（150ms, ease_out）。
 *
 * @param root 设置页面根对象（未使用）。
 */
void ui_app_settings_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_settings_panel, -260);
    start_y_anim(g_settings_panel, -260, 48, 150, 0, lv_anim_path_ease_out, NULL, NULL);
}

/**
 * @brief 停止设置页面动画并删除 root。
 *
 * @param root    设置页面根对象。
 * @param done_cb 退场完成回调。
 */
void ui_app_settings_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    lv_anim_del(g_settings_panel, anim_set_y);
    ui_event_unregister_settings(&g_settings_view);
    lv_obj_delete(root);
    if(done_cb != NULL) {
        done_cb(NULL);
    }
}
