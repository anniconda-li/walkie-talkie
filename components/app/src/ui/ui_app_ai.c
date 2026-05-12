/**
 * @file ui_app_ai.c
 * @brief AI 问答页面 UI——创建控件、入场/退场动画。
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
 * - 入场：answer_panel 从上方滑入(270ms)、4 根柱子依次弹入(240ms)、按钮从下方弹入(250ms)
 * - 退场：所有元素反向滑出，动画结束后删除 root 对象并调用 done_cb
 */
#include "ui_app_ai.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_i18n.h"

/**
 * @brief 页面退场上下文——用于在动画结束后清理。
 *
 * 退场动画是异步的，需要保存 root 和回调指针，
 * 在动画完成时删除 UI 对象并调用回调。
 */
typedef struct {
    lv_anim_completed_cb_t done_cb; /**< 退场完成后的回调（如切换到新页面） */
    lv_obj_t * root;                /**< AI 页面根对象，动画结束后删除 */
} app_exit_ctx_t;

/** @brief AI 页面全局视图对象（含 answer_label, ask_button, voice_bars, speaking 状态） */
static ui_ai_view_t g_ai_view;

/** @brief AI 回答面板（深灰圆角矩形） */
static lv_obj_t *g_answer_panel;

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

/**
 * @brief 退场动画完成回调：删除 root 对象 → 释放上下文 → 调用上层回调。
 *
 * 这是页面切换的关键环节：
 * 1. 删除旧页面的 LVGL 对象树（释放内存）
 * 2. 调用 ui_shell 传入的 done_cb（通常是 app_switch_done_cb）
 * 3. app_switch_done_cb 会创建新页面并播放其入场动画
 *
 * @param a LVGL 动画对象，user_data 指向 app_exit_ctx_t。
 */
static void app_exit_done_cb(lv_anim_t *a)
{
    app_exit_ctx_t *ctx = (app_exit_ctx_t *)lv_anim_get_user_data(a);

    if(ctx->root) {
        lv_obj_delete(ctx->root);
    }

    if(ctx->done_cb) {
        ctx->done_cb(a);
    }

    lv_free(ctx);
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
 * └── ask_button (圆角按钮, y=262, 120×42)
 *     └── ask_label (居中文本 "按住提问")
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
    lv_obj_set_style_border_color(g_answer_panel, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_border_width(g_answer_panel, 1, 0);
    lv_obj_set_style_pad_all(g_answer_panel, 12, 0);

    g_ai_view.answer_label = lv_label_create(g_answer_panel);
    lv_label_set_text(g_ai_view.answer_label, ui_i18n_text(UI_TEXT_AI_IDLE));
    lv_label_set_long_mode(g_ai_view.answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_ai_view.answer_label, 180);
    lv_obj_set_style_text_color(g_ai_view.answer_label, lv_color_make(0xEA, 0xEA, 0xEA), 0);
    lv_obj_set_pos(g_ai_view.answer_label, 0, 0);

    /* 4 根录音动画柱（初始隐藏，录音时才显示） */
    for(int32_t i = 0; i < 4; i++) {
        g_ai_view.voice_bars[i] = lv_obj_create(root);
        lv_obj_remove_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(g_ai_view.voice_bars[i], 7, 18 + (i % 2) * 12);
        lv_obj_set_pos(g_ai_view.voice_bars[i], 88 + i * 16, 238 - (i % 2) * 6);
        lv_obj_set_style_radius(g_ai_view.voice_bars[i], 3, 0);
        lv_obj_set_style_bg_color(g_ai_view.voice_bars[i], lv_color_make(0xFF, 0x66, 0x00), 0);
        lv_obj_set_style_border_width(g_ai_view.voice_bars[i], 0, 0);
        lv_obj_add_flag(g_ai_view.voice_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* "按住提问" 按钮 */
    g_ai_view.ask_button = lv_button_create(root);
    lv_obj_set_pos(g_ai_view.ask_button, 60, 262);
    lv_obj_set_size(g_ai_view.ask_button, 120, 42);
    lv_obj_set_style_radius(g_ai_view.ask_button, 21, 0);
    lv_obj_set_style_bg_color(g_ai_view.ask_button, lv_color_make(0x36, 0x36, 0x36), 0);
    lv_obj_set_style_border_color(g_ai_view.ask_button, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_border_width(g_ai_view.ask_button, 1, 0);

    g_ai_view.ask_label = lv_label_create(g_ai_view.ask_button);
    lv_label_set_text(g_ai_view.ask_label, ui_i18n_text(UI_TEXT_AI_ASK));
    lv_obj_set_style_text_color(g_ai_view.ask_label, lv_color_white(), 0);
    lv_obj_center(g_ai_view.ask_label);

    g_ai_view.speaking = false;
    ui_event_register_ai(&g_ai_view);
    return root;
}

/**
 * @brief 播放 AI 页面入场动画。
 *
 * 动画顺序（各元素依次出现，总时长约 325ms）：
 * 1. answer_panel 从上方(-180)滑入到 y=56（270ms, ease_out）
 * 2. 4 根录音柱依次从底部弹入（240ms, 各延迟 40/58/76/94ms）
 * 3. ask_button 从下方弹入（250ms, 延迟 75ms）
 *
 * @param root AI 页面根对象（未使用，直接操作全局对象）。
 */
void ui_app_ai_enter(lv_obj_t * root)
{
    (void)root;

    lv_obj_set_y(g_answer_panel, -180);
    lv_obj_set_y(g_ai_view.ask_button, UI_SCREEN_HEIGHT + 12);
    for(int32_t i = 0; i < 4; i++) {
        lv_obj_set_y(g_ai_view.voice_bars[i], UI_SCREEN_HEIGHT + 12);
    }

    start_y_anim(g_answer_panel, -180, 56, 270, 0, lv_anim_path_ease_out, NULL, NULL);
    for(int32_t i = 0; i < 4; i++) {
        int32_t target_y = 238 - (i % 2) * 6;
        start_y_anim(g_ai_view.voice_bars[i],
                     UI_SCREEN_HEIGHT + 12,
                     target_y,
                     240,
                     (uint32_t)(40 + i * 18),
                     lv_anim_path_ease_out,
                     NULL,
                     NULL);
    }
    start_y_anim(g_ai_view.ask_button, UI_SCREEN_HEIGHT + 12, 262, 250, 75, lv_anim_path_ease_out, NULL, NULL);
}

/**
 * @brief 播放 AI 页面退场动画并在完成后删除 root。
 *
 * 动画与入场相反：
 * 1. answer_panel 向上滑出（220ms, ease_in）
 * 2. 4 根柱子依次向下滑出（210ms, 各延迟 25/40/55/70ms）
 * 3. ask_button 向下滑出（220ms, 延迟 70ms）
 *
 * 最后一个动画（ask_button）完成时触发 app_exit_done_cb：
 * 删除 root → 调用 done_cb（通常触发新页面入场）
 *
 * @param root    AI 页面根对象（退场后删除）。
 * @param done_cb 退场完成回调（通常由 ui_shell 传入，用于切换到新页面）。
 */
void ui_app_ai_exit(lv_obj_t * root, lv_anim_completed_cb_t done_cb)
{
    app_exit_ctx_t *ctx = lv_malloc(sizeof(app_exit_ctx_t));
    if(ctx == NULL) {
        lv_obj_delete(root);
        if(done_cb != NULL) {
            done_cb(NULL);
        }
        return;
    }
    ctx->done_cb = done_cb;
    ctx->root = root;

    start_y_anim(g_answer_panel, lv_obj_get_y(g_answer_panel), -180, 220, 0, lv_anim_path_ease_in, NULL, NULL);
    for(int32_t i = 0; i < 4; i++) {
        start_y_anim(g_ai_view.voice_bars[i],
                     lv_obj_get_y(g_ai_view.voice_bars[i]),
                     UI_SCREEN_HEIGHT + 12,
                     210,
                     (uint32_t)(25 + i * 15),
                     lv_anim_path_ease_in,
                     NULL,
                     NULL);
    }
    start_y_anim(g_ai_view.ask_button,
                 lv_obj_get_y(g_ai_view.ask_button),
                 UI_SCREEN_HEIGHT + 12,
                 220,
                 70,
                 lv_anim_path_ease_in,
                 app_exit_done_cb,
                 ctx);
}
