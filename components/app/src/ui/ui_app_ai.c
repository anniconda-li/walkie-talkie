/**
 * @file ui_app_ai.c
 * @brief AI 问答页面 UI——创建控件、轻量入场动画。
 *
 * ## 页面布局（240×280 屏幕）
 * ```
 * ┌──────────────────────────┐
 * │    状态栏（shell 管理）     │  y=0..30
 * ├──────────────────────────┤
 * │  ┌────────────────────┐  │
 * │  │  AI 回答显示区域     │  │  y=56, w=204, h=170
 * │  │  "AI 回答会显示..."  │  │
 * │  └────────────────────┘  │
 * │                          │
 * │   ▊ ▊ ▊ ▊  ← 录音动画柱  │  y=238
 * │                          │
 * │     [ 按住提问 ]         │  y=262, w=120, h=42
 * └──────────────────────────┘
 * ```
 *
 * ## 交互
 * - 长按 [按住提问] 按钮 → 触发 ai_question_started 回调 → 开始录音
 * - 松开 → 触发 ai_question_stopped 回调 → 停止录音 + 启动处理
 * - 录音期间 4 根橙色柱子上下伸缩动画
 *
 * ## 动画说明
 * - 入场：answer_panel 从上方短距离滑入，按钮直接就位
 * - 退场：停止页面动画后直接删除 root 对象并调用 done_cb
 */
#include "ui_app_ai.h"
#include "ui.h"
#include "ui_assets.h"
#include "ui_event.h"
#include "ui_font.h"
#include "ui_i18n.h"
#include "ui_theme.h"

/** @brief AI 页面全局视图对象（含 answer_label, ask_button, voice_bars, speaking 状态） */
static ui_ai_view_t g_ai_view;

/** @brief AI 回答面板（深灰圆角矩形） */
static lv_obj_t *g_answer_panel;

#define AI_ACTION_BTN_Y      256
#define AI_ACTION_BTN_W      104
#define AI_ACTION_BTN_H      48
#define AI_CAMERA_BTN_X      12
#define AI_ASK_BTN_X         124
#define AI_ACTION_BTN_RADIUS 24
#define AI_AUDIO_BTN_SIZE    40

/* ---- LVGL 动画回调函数 ---- */

/** @brief LVGL 动画回调：设置对象的 y 坐标。 */
static void anim_set_y(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

/**
 * @brief 启动 Y 轴位移动画。
 *
 * @param obj       目标 LVGL 对象。
 * @param from      起始 y 坐标。
 * @param to        目标 y 坐标。
 * @param duration  动画时长（ms）。
 * @param delay     延迟启动（ms）。
 * @param path_cb   缓动函数（ease_out/ease_in/ease_in_out）。
 * @param done_cb   动画完成回调（可为 NULL）。
 * @param user_data 回调用户数据。
 */
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

/* ==========================================================================
 * 页面创建
 * ========================================================================== */

/**
 * @brief 创建 AI 问答页面完整 UI。
 *
 * ## 控件层级
 * root (全屏透明容器)
 * ├── g_answer_panel (深灰圆角矩形, y=56, 204×170)
 * │   └── answer_label (多行文本, 宽度 180)
 * ├── voice_bars[4] (4 根橙色柱子, 初始隐藏)
 * ├── camera_button (圆角图标按钮, y=256, 104×48)
 * └── ask_button (圆角图标按钮, y=256, 104×48)
 *
 * 创建完成后调用 ui_event_register_ai() 注册长按事件。
 *
 * @param parent 挂载的父容器（g_app_content_root）。
 * @return AI 页面根对象。
 */
lv_obj_t * ui_app_ai_create(lv_obj_t * parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);

    /* AI 回答显示面板 */
    g_answer_panel = lv_obj_create(root);
    lv_obj_remove_flag(g_answer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_answer_panel, 18, 56);
    lv_obj_set_size(g_answer_panel, 204, 170);
    lv_obj_set_style_radius(g_answer_panel, 16, 0);
    lv_obj_set_style_bg_color(g_answer_panel, lv_color_make(0x22, 0x22, 0x22), 0);
    lv_obj_set_style_bg_opa(g_answer_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_answer_panel, UI_COLOR_AI, 0);
    lv_obj_set_style_border_width(g_answer_panel, 1, 0);
    lv_obj_set_style_pad_all(g_answer_panel, 12, 0);

    g_ai_view.answer_label = lv_label_create(g_answer_panel);
    lv_label_set_text(g_ai_view.answer_label, ui_i18n_text(UI_TEXT_AI_IDLE));
    lv_label_set_long_mode(g_ai_view.answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_ai_view.answer_label, 180);
    lv_obj_set_style_text_color(g_ai_view.answer_label, lv_color_make(0xEA, 0xEA, 0xEA), 0);
    lv_obj_set_style_text_font(g_ai_view.answer_label, ui_font_normal(), 0);
    lv_obj_set_pos(g_ai_view.answer_label, 0, 0);

    g_ai_view.audio_button = lv_button_create(root);
    lv_obj_set_pos(g_ai_view.audio_button, 100, 210);
    lv_obj_set_size(g_ai_view.audio_button, AI_AUDIO_BTN_SIZE, AI_AUDIO_BTN_SIZE);
    lv_obj_set_style_radius(g_ai_view.audio_button, AI_AUDIO_BTN_SIZE / 2, 0);
    lv_obj_set_style_bg_color(g_ai_view.audio_button, lv_color_make(0x3A, 0x3A, 0x3A), 0);
    lv_obj_set_style_border_width(g_ai_view.audio_button, 1, 0);
    lv_obj_set_style_border_color(g_ai_view.audio_button, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_set_style_shadow_width(g_ai_view.audio_button, 0, 0);
    lv_obj_add_state(g_ai_view.audio_button, LV_STATE_DISABLED);

    g_ai_view.audio_label = lv_label_create(g_ai_view.audio_button);
    lv_label_set_text(g_ai_view.audio_label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(g_ai_view.audio_label, lv_color_make(0x9A, 0x9A, 0x9A), 0);
    lv_obj_set_style_text_font(g_ai_view.audio_label, ui_font_normal(), 0);
    lv_obj_center(g_ai_view.audio_label);
    lv_obj_remove_flag(g_ai_view.audio_label, LV_OBJ_FLAG_CLICKABLE);

    /* 4 根录音动画柱（初始隐藏，录音时才显示） */
    for(int32_t i = 0; i < 4; i++) {
        g_ai_view.voice_bars[i] = lv_obj_create(root);
        lv_obj_remove_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(g_ai_view.voice_bars[i], 7, 18 + (i % 2) * 12);
        lv_obj_set_pos(g_ai_view.voice_bars[i], 88 + i * 16, 238 - (i % 2) * 6);
        lv_obj_set_style_radius(g_ai_view.voice_bars[i], 3, 0);
        lv_obj_set_style_bg_color(g_ai_view.voice_bars[i], UI_COLOR_AI, 0);
        lv_obj_set_style_border_width(g_ai_view.voice_bars[i], 0, 0);
        lv_obj_add_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* "拍照" 按钮 */
    g_ai_view.camera_button = lv_button_create(root);
    lv_obj_set_pos(g_ai_view.camera_button, AI_CAMERA_BTN_X, AI_ACTION_BTN_Y);
    lv_obj_set_size(g_ai_view.camera_button, AI_ACTION_BTN_W, AI_ACTION_BTN_H);
    lv_obj_set_style_radius(g_ai_view.camera_button, AI_ACTION_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ai_view.camera_button, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_border_color(g_ai_view.camera_button, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_border_width(g_ai_view.camera_button, 1, 0);

    g_ai_view.camera_icon = lv_image_create(g_ai_view.camera_button);
    lv_image_set_src(g_ai_view.camera_icon, &icon_ai_camera);
    lv_image_set_scale(g_ai_view.camera_icon, 160);
    lv_obj_center(g_ai_view.camera_icon);
    lv_obj_remove_flag(g_ai_view.camera_icon, LV_OBJ_FLAG_CLICKABLE);

    /* "按住提问" 按钮 */
    g_ai_view.ask_button = lv_button_create(root);
    lv_obj_set_pos(g_ai_view.ask_button, AI_ASK_BTN_X, AI_ACTION_BTN_Y);
    lv_obj_set_size(g_ai_view.ask_button, AI_ACTION_BTN_W, AI_ACTION_BTN_H);
    lv_obj_set_style_radius(g_ai_view.ask_button, AI_ACTION_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ai_view.ask_button, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_bg_color(g_ai_view.ask_button, lv_color_make(0x1C, 0x4A, 0x54), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(g_ai_view.ask_button, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_border_width(g_ai_view.ask_button, 1, 0);

    g_ai_view.ask_icon = lv_image_create(g_ai_view.ask_button);
    lv_image_set_src(g_ai_view.ask_icon, &icon_ai_mic);
    lv_image_set_scale(g_ai_view.ask_icon, 160);
    lv_obj_center(g_ai_view.ask_icon);
    lv_obj_remove_flag(g_ai_view.ask_icon, LV_OBJ_FLAG_CLICKABLE);

    g_ai_view.speaking = false;
    ui_event_register_ai(&g_ai_view);
    return root;
}

/**
 * @brief 播放 AI 页面轻量入场动画。
 *
 * 只动画回答面板，按钮和录音柱直接就位，避免切页时多个对象同时重绘。
 *
 * @param root AI 页面根对象（未使用，直接操作全局对象）。
 */
void ui_app_ai_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_answer_panel, -180);
    lv_obj_set_y(g_ai_view.audio_button, 210);
    lv_obj_set_y(g_ai_view.camera_button, AI_ACTION_BTN_Y);
    lv_obj_set_y(g_ai_view.ask_button, AI_ACTION_BTN_Y);
    for(int32_t i = 0; i < 4; i++) {
        lv_obj_set_y(g_ai_view.voice_bars[i], 238 - (i % 2) * 6);
    }

    start_y_anim(g_answer_panel, -180, 56, 150, 0, lv_anim_path_ease_out, NULL, NULL);
}

/**
 * @brief 停止 AI 页面动画并删除 root。
 *
 * @param root    AI 页面根对象（退场后删除）。
 * @param done_cb 退场完成回调（通常由 ui_shell 传入，用于切换到新页面）。
 */
void ui_app_ai_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    ui_event_unregister_ai(&g_ai_view);

    lv_anim_del(g_answer_panel, anim_set_y);
    lv_anim_del(g_ai_view.audio_button, anim_set_y);
    lv_anim_del(g_ai_view.camera_button, anim_set_y);
    lv_anim_del(g_ai_view.ask_button, anim_set_y);
    for(int32_t i = 0; i < 4; i++) {
        lv_anim_del(g_ai_view.voice_bars[i], anim_set_y);
    }
    lv_obj_delete(root);
    if(done_cb != NULL) {
        done_cb(NULL);
    }
}
