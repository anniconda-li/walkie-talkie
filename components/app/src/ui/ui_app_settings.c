/**
 * @file ui_app_settings.c
 * @brief 设置页面 UI——音量、固件版本信息。
 *
 * ## 页面布局（240×280 屏幕）
 * ```
 * ┌──────────────────────────┐
 * │    状态栏（shell 管理）    │  y=0..30
 * ├──────────────────────────┤
 * │  ┌────────────────────┐  │
 * │  │ 音量大小            │  │
 * │  │ [══════●═══════] 56%│  │
 * │  │                    │  │
 * │  │ 固件版本            │  │
 * │  │ v1.0.0              │  │
 * │  └────────────────────┘  │
 * └──────────────────────────┘
 * ```
 *
 * ## 交互
 * - 音量滑块：LV_EVENT_VALUE_CHANGED → 回调到 app_business
 *
 * ## 动画说明
 * - 入场：面板整体短距离滑入
 * - 退场：停止页面动画后直接删除 root → 调用 done_cb
 */
#include "ui_app_settings.h"
#include "ui.h"
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
    lv_obj_set_pos(label, 2, y);
    lv_obj_set_style_text_color(label, lv_color_make(0xD8, 0xD8, 0xD8), 0);
    return label;
}

/**
 * @brief 创建范围滑块（0-100）。
 *
 * 主轨道深灰(#454545)，已选部分橙色(#FF6600)，滑块白色。
 *
 * @param parent 父容器。
 * @param y      垂直位置。
 * @param value  初始值。
 * @return 滑块 LVGL 对象。
 */
static lv_obj_t *create_slider(lv_obj_t *parent, int32_t y, int32_t value)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, 2, y);
    lv_obj_set_size(slider, 188, 10);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x45, 0x45, 0x45), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COLOR_SETTINGS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    return slider;
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

    /* 音量行 */
    g_settings_view.volume_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_VOLUME), 0);
    g_settings_view.volume_slider = create_slider(g_settings_panel, 30, 56);

    /* 固件版本信息行（只读标签） */
    g_settings_view.firmware_label = create_caption(g_settings_panel, ui_i18n_text(UI_TEXT_SETTINGS_FIRMWARE), 68);

    g_settings_view.version_label = lv_label_create(g_settings_panel);
    set_firmware_version_label(g_settings_view.version_label);
    lv_obj_set_pos(g_settings_view.version_label, 2, 92);
    lv_obj_set_style_text_color(g_settings_view.version_label, lv_color_make(0xD8, 0xD8, 0xD8), 0);

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
    lv_obj_delete(root);
    if(done_cb != NULL) {
        done_cb(NULL);
    }
}
