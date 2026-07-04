/**
 * @file ui_event.c
 * @brief UI 事件桥接——LVGL 控件事件到业务回调的转换层。
 *
 * ## 设计原则
 * UI 层只负责控件创建、动画和用户交互检测，不感知任何硬件和业务逻辑。
 * 所有"用户做了什么"都通过 ui_event_callbacks_t 回调函数指针桥接到 app_business 层，
 * 由 app_business 转发给具体的 app 模块。
 *
 * ## 回调注册流程
 * 1. app_business.c 在启动时调用 ui_event_set_callbacks(&callbacks) 注册全局回调
 * 2. 各 UI 页面（ui_app_*.c）在创建控件后调用 ui_event_register_*() 绑定 LVGL 事件
 * 3. 用户操作 → LVGL 事件 → 本文件的事件处理函数 → 调用 g_callbacks.xxx()
 *
 * ## 当前支持的事件
 * - 对讲：频道切换（+/- 按钮）、PTT 按下/松开（长按/释放）
 * - 相机：拍照、上传、重拍
 * - AI：提问开始/停止（长按/释放）
 * - 设置：音量滑块变化
 */
#include "ui_event.h"
#include "ui_assets.h"
#include "app_config.h"
#include "ui_font.h"
#include "ui_i18n.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include <stdbool.h>
#include <string.h>

#define INTERCOM_CHANNEL_MIN 1
#define INTERCOM_CHANNEL_MAX 32
#define PTT_RING_BASE_SIZE 48
#define PTT_RING_MAX_SIZE 132
#define AI_BAR_BASE_H 12
#define VOLUME_OVERLAY_HIDE_MS 1200u

/**
 * @brief 全局 UI 事件回调集合。
 *
 * 由 app_business.c 在启动时通过 ui_event_set_callbacks() 注册。
 * UI 控件的 LVGL 事件处理函数通过此结构体调用业务回调。
 */
static ui_event_callbacks_t g_callbacks;
static ui_ai_view_t *g_ai_view = NULL;
static ui_settings_view_t *g_settings_view = NULL;
static lv_timer_t *g_ai_wait_timer = NULL;
static lv_timer_t *g_settings_refresh_timer = NULL;
static uint8_t g_ai_waiting = 0u;
static uint8_t g_ai_wait_dot_count = 0u;
static ui_text_id_t g_ai_message_id = UI_TEXT_AI_IDLE;
static ui_ai_audio_btn_state_t g_ai_audio_btn_state = UI_AI_AUDIO_BTN_HIDDEN;
static lv_obj_t *g_ai_cancel_label = NULL;
static int32_t g_settings_volume = 80;
static uint8_t g_settings_volume_syncing = 0u;
static int32_t g_settings_brightness = 80;
static uint8_t g_settings_brightness_syncing = 0u;
static ui_settings_network_mode_t g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
static lv_obj_t *g_volume_overlay = NULL;
static lv_obj_t *g_volume_slider = NULL;
static lv_timer_t *g_volume_hide_timer = NULL;

static void ai_answer_scroll_top(ui_ai_view_t *view);

/* ==========================================================================
 * 通用 UI 工具函数
 * ========================================================================== */

/** @brief 设置 LVGL 对象的隐藏/可见状态。 */
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

/* ==========================================================================
 * 对讲页面事件处理
 * ========================================================================== */

/** @brief 刷新频道标签文字（"CH 01" ~ "CH 32" 格式）。 */
static void refresh_intercom_channel(ui_intercom_view_t *view)
{
    char text[16];

    if(view == NULL || view->channel_label == NULL) {
        return;
    }

    lv_snprintf(text, sizeof(text), "CH %02ld", (long)view->channel);
    lv_label_set_text(view->channel_label, text);
}

/* ---- 动画回调函数（由 LVGL 动画引擎调用） ---- */

/** @brief LVGL 动画回调：设置对象尺寸（用于 PTT 波纹效果）。 */
static void anim_set_ring_size(void *obj, int32_t size)
{
    lv_obj_set_size((lv_obj_t *)obj, size, size);
    lv_obj_center((lv_obj_t *)obj);
}

/** @brief LVGL 动画回调：设置对象透明度（用于 PTT 波纹渐隐效果）。 */
static void anim_set_opa(void *obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

/** @brief LVGL 动画回调：设置 AI 录音柱高度（用于录音动效）。 */
static void anim_set_ai_bar_h(void *obj, int32_t h)
{
    lv_obj_set_height((lv_obj_t *)obj, h);
}

/**
 * @brief 启动 PTT 波纹动画。
 *
 * 3 个同心圆从 48px 扩张到 132px 同时透明→不透明，
 * 每层延迟 240ms 依次启动，产生涟漪效果。
 * LV_ANIM_REPEAT_INFINITE 表示无限循环。
 */
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

/** @brief 停止 PTT 波纹动画，恢复默认状态。 */
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

/**
 * @brief 切换对讲页面的"说话中"视觉状态。
 *
 * PTT 按下时：显示 3 层涟漪波纹 + 隐藏频道标签 + 按钮变橙色
 * PTT 松开时：停止波纹 + 显示频道标签 + 按钮恢复灰色
 */
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
                                  talking ? UI_COLOR_INTERCOM : lv_color_make(0x3A, 0x3A, 0x3A),
                                  0);
    }
}

/** @brief 切换频道（delta = +1 或 -1），钳位到 1-32 范围并触发回调。 */
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

/* ---- LVGL 事件回调（注册到具体控件上） ---- */

/** @brief 频道减少按钮事件：单击或长按连续减频道。 */
static void intercom_dec_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        intercom_change_channel((ui_intercom_view_t *)lv_event_get_user_data(e), -1);
    }
}

/** @brief 频道增加按钮事件：单击或长按连续加频道。 */
static void intercom_inc_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        intercom_change_channel((ui_intercom_view_t *)lv_event_get_user_data(e), 1);
    }
}

/**
 * @brief PTT 按钮事件：长按开始说话，松开停止。
 *
 * LV_EVENT_LONG_PRESSED —— 用户按住不放（LVGL 内部计时约 400ms 触发）
 * LV_EVENT_RELEASED —— 用户松手
 * LV_EVENT_PRESS_LOST —— 手指滑出按钮区域
 */
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

/* ==========================================================================
 * 相机页面事件处理
 * ========================================================================== */

/** @brief 拍照/重新预览按钮：在实时预览和定格画面之间切换。 */
static void camera_capture_event_cb(lv_event_t *e)
{
    ui_camera_view_t *view = (ui_camera_view_t *)lv_event_get_user_data(e);

    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL) {
        return;
    }

    if(view->frozen) {
        view->frozen = false;
        lv_image_set_src(view->capture_icon, &icon_camera_capture);
        lv_obj_add_state(view->upload_button, LV_STATE_DISABLED);

        if(g_callbacks.camera_retake_requested != NULL) {
            g_callbacks.camera_retake_requested();
        }
    }
    else {
        view->frozen = true;
        lv_image_set_src(view->capture_icon, &icon_camera_retake);
        lv_obj_remove_state(view->upload_button, LV_STATE_DISABLED);

        if(g_callbacks.camera_capture_requested != NULL) {
            g_callbacks.camera_capture_requested();
        }
    }
}

/** @brief 上传按钮：触发上传并切到 AI 问答页面。 */
static void camera_upload_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int ret = -1;
        if(g_callbacks.camera_upload_requested != NULL) {
            ret = g_callbacks.camera_upload_requested();
        }
        if(ret == 0) {
            ui_event_set_ai_message(UI_TEXT_AI_IMAGE_UPLOADING);
            ui_event_set_ai_waiting(true);
        }
        else if(ret == -3) {
            ui_event_set_ai_waiting(false);
            ui_event_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            ui_event_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_BUSY);
        }
        else {
            ui_event_set_ai_waiting(false);
            ui_event_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            ui_event_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        }
        ui_shell_switch_to(UI_APP_ID_AI);
    }
}

/** @brief Return 按钮：回到 AI 问答界面。 */
static void camera_retake_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_shell_switch_to(UI_APP_ID_AI);
    }
}

/* ==========================================================================
 * AI 页面事件处理
 * ========================================================================== */

static bool ai_cancel_entry_active(void)
{
    return g_ai_waiting != 0u;
}

static const char *ai_waiting_text(void)
{
    if(g_ai_message_id == UI_TEXT_AI_IMAGE_UPLOADING) {
        switch(g_ai_wait_dot_count) {
            case 1:
                return "图片上传中.";
            case 2:
                return "图片上传中..";
            default:
                return ui_i18n_text(UI_TEXT_AI_IMAGE_UPLOADING);
        }
    }

    switch(g_ai_wait_dot_count) {
        case 1:
            return "正在识别和思考.";
        case 2:
            return "正在识别和思考..";
        default:
            return "正在识别和思考...";
    }
}

static void ai_apply_cancel_entry(ui_ai_view_t *view)
{
    if(view == NULL || view->ask_button == NULL) {
        return;
    }

    bool active = ai_cancel_entry_active();
    if(active) {
        if(view->ask_icon != NULL) {
            lv_obj_add_flag(view->ask_icon, LV_OBJ_FLAG_HIDDEN);
        }
        if(g_ai_cancel_label == NULL) {
            g_ai_cancel_label = lv_label_create(view->ask_button);
            lv_obj_set_style_text_font(g_ai_cancel_label, ui_font_normal(), 0);
            lv_obj_remove_flag(g_ai_cancel_label, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_label_set_text(g_ai_cancel_label, "中止");
        lv_obj_set_style_text_color(g_ai_cancel_label, lv_color_white(), 0);
        lv_obj_center(g_ai_cancel_label);
        lv_obj_remove_flag(g_ai_cancel_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(view->ask_button, UI_COLOR_AI, 0);
        lv_obj_set_style_bg_color(view->ask_button, lv_color_make(0x1C, 0x4A, 0x54), 0);
    }
    else {
        if(g_ai_cancel_label != NULL) {
            lv_obj_add_flag(g_ai_cancel_label, LV_OBJ_FLAG_HIDDEN);
        }
        if(view->ask_icon != NULL) {
            lv_obj_remove_flag(view->ask_icon, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_border_color(view->ask_button, lv_color_make(0x88, 0x88, 0x88), 0);
        lv_obj_set_style_bg_color(view->ask_button,
                                  view->speaking ? lv_color_make(0x1C, 0x4A, 0x54) : lv_color_make(0x36, 0x36, 0x36),
                                  0);
    }
}

/**
 * @brief 切换 AI 页面的"说话中/空闲"视觉状态。
 *
 * 说话中：
 * - 按钮变橙色（#FF6600）
 * - 显示 4 根录音柱，每根做上下伸缩动画（循环往复，模拟音量波形）
 * - 标签改为"正在聆听..."
 *
 * 空闲：
 * - 按钮恢复深灰色
 * - 隐藏录音柱
 * - 标签恢复"按住提问"
 *
 * 录音柱动画参数：
 * - 基础高度 12px，伸缩到 24/36px（奇偶交替）
 * - 周期 220ms-340ms（各柱有相位差，产生波浪效果）
 * - LV_ANIM_REPEAT_INFINITE 无限循环
 */
static void ai_set_speaking(ui_ai_view_t *view, bool speaking)
{
    if(view == NULL) {
        return;
    }

    if(speaking && g_ai_waiting != 0u) {
        ui_event_set_ai_waiting(false);
    }

    view->speaking = speaking;
    lv_obj_set_style_bg_color(view->ask_button,
                              speaking ? lv_color_make(0x1C, 0x4A, 0x54) : lv_color_make(0x36, 0x36, 0x36),
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
    if(speaking) {
        lv_label_set_text(view->answer_label, ui_i18n_text(UI_TEXT_AI_LISTENING));
        ai_answer_scroll_top(view);
    }
    else if(g_ai_waiting == 0u) {
        lv_label_set_text(view->answer_label, ui_i18n_text(g_ai_message_id));
        ai_answer_scroll_top(view);
    }
    ai_apply_cancel_entry(view);
}

static void ai_wait_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if(g_ai_view == NULL || g_ai_view->answer_label == NULL || g_ai_waiting == 0u) {
        return;
    }

    g_ai_wait_dot_count = (uint8_t)((g_ai_wait_dot_count % 3u) + 1u);
    lv_label_set_text(g_ai_view->answer_label, ai_waiting_text());
}

static void ai_answer_scroll_top(ui_ai_view_t *view)
{
    if(view != NULL && view->answer_panel != NULL) {
        lv_obj_scroll_to_y(view->answer_panel, 0, LV_ANIM_OFF);
    }
}

/**
 * @brief AI 提问按钮事件：长按开始录音，松开停止并触发问答。
 *
 * 与 PTT 相同的事件机制：
 * - LV_EVENT_LONG_PRESSED → 触发录音开始
 * - LV_EVENT_RELEASED / LV_EVENT_PRESS_LOST → 触发录音停止
 */
static void ai_ask_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ui_ai_view_t *view = (ui_ai_view_t *)lv_event_get_user_data(e);

    if(g_ai_waiting != 0u) {
        if(g_callbacks.ai_cancel_requested != NULL) {
            if(ai_cancel_entry_active() && code == LV_EVENT_SHORT_CLICKED) {
                g_callbacks.ai_cancel_requested();
                ui_event_set_ai_message(UI_TEXT_AI_IDLE);
                ui_event_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            }
        }
        return;
    }

    if(code == LV_EVENT_SHORT_CLICKED) {
        return;
    }

    if(code == LV_EVENT_LONG_PRESSED) {
        ai_set_speaking(view, true);
        if(g_callbacks.ai_question_started != NULL) {
            g_callbacks.ai_question_started();
        }
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        bool was_speaking = view != NULL && view->speaking;
        ai_set_speaking(view, false);
        if(was_speaking && g_callbacks.ai_question_stopped != NULL) {
            g_callbacks.ai_question_stopped();
            ui_event_set_ai_waiting(true);
        }
    }
}

/** @brief AI 页拍照按钮：切换到相机页面。 */
static void ai_camera_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_shell_switch_to(UI_APP_ID_CAMERA);
    }
}

static void ai_audio_play_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    APP_LOGI("AI-UI", "audio button clicked");
    if(g_ai_audio_btn_state == UI_AI_AUDIO_BTN_PLAYING) {
        if(g_callbacks.ai_reply_stop_requested != NULL) {
            g_callbacks.ai_reply_stop_requested();
        }
        return;
    }

    if(g_ai_audio_btn_state != UI_AI_AUDIO_BTN_READY) {
        APP_LOGW("AI-UI", "play button disabled, audio not ready");
        return;
    }

    if(g_callbacks.ai_reply_play_requested != NULL) {
        g_callbacks.ai_reply_play_requested();
    }
}

static void ai_apply_audio_button_state(ui_ai_view_t *view, ui_ai_audio_btn_state_t state)
{
    if(view == NULL || view->audio_button == NULL || view->audio_label == NULL) {
        return;
    }

    g_ai_audio_btn_state = state;
    ai_apply_cancel_entry(view);
    lv_label_set_text(view->audio_label, state == UI_AI_AUDIO_BTN_PLAYING ? LV_SYMBOL_STOP : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(view->audio_label, ui_font_normal(), 0);

    if(state == UI_AI_AUDIO_BTN_HIDDEN) {
        lv_obj_add_flag(view->audio_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(view->audio_button, LV_STATE_DISABLED);
        return;
    }

    lv_obj_remove_flag(view->audio_button, LV_OBJ_FLAG_HIDDEN);

    if(state == UI_AI_AUDIO_BTN_READY) {
        lv_obj_remove_state(view->audio_button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(view->audio_button, UI_COLOR_AI, 0);
        lv_obj_set_style_border_color(view->audio_button, lv_color_make(0x68, 0xD8, 0xE8), 0);
        lv_obj_set_style_text_color(view->audio_label, lv_color_white(), 0);
        return;
    }

    if(state == UI_AI_AUDIO_BTN_PLAYING) {
        lv_obj_remove_state(view->audio_button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(view->audio_button, lv_color_make(0x54, 0x1C, 0x1C), 0);
        lv_obj_set_style_border_color(view->audio_button, lv_color_make(0xE8, 0x68, 0x68), 0);
        lv_obj_set_style_text_color(view->audio_label, lv_color_white(), 0);
        return;
    }

    lv_obj_add_state(view->audio_button, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(view->audio_button, lv_color_make(0x3A, 0x3A, 0x3A), 0);
    lv_obj_set_style_border_color(view->audio_button, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_set_style_text_color(view->audio_label, lv_color_make(0x9A, 0x9A, 0x9A), 0);
}

static int32_t normalize_volume_step(int32_t volume)
{
    if(volume < 0) {
        return 0;
    }
    if(volume > 100) {
        return 100;
    }

    return ((volume + 5) / 10) * 10;
}

static int32_t normalize_percent(int32_t value)
{
    if(value < 0) {
        return 0;
    }
    if(value > 100) {
        return 100;
    }
    return value;
}

static void settings_refresh_brightness_label(ui_settings_view_t *view)
{
    if(view == NULL || view->brightness_value_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(view->brightness_value_label, "%ld%%", (long)g_settings_brightness);
}

static void settings_brightness_slider_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || g_settings_brightness_syncing != 0u) {
        return;
    }

    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int32_t brightness = normalize_percent(lv_slider_get_value(slider));
    g_settings_brightness = brightness;
    settings_refresh_brightness_label(view);
    if(g_callbacks.settings_brightness_changed != NULL) {
        g_callbacks.settings_brightness_changed(brightness);
    }
}

static void volume_overlay_hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(g_volume_overlay != NULL) {
        lv_obj_add_flag(g_volume_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_volume_hide_timer != NULL) {
        lv_timer_pause(g_volume_hide_timer);
    }
}

static void volume_overlay_refresh_hide_timer(void)
{
    if(g_volume_hide_timer == NULL) {
        g_volume_hide_timer = lv_timer_create(volume_overlay_hide_timer_cb, VOLUME_OVERLAY_HIDE_MS, NULL);
    }
    lv_timer_reset(g_volume_hide_timer);
    lv_timer_resume(g_volume_hide_timer);
}

static void volume_overlay_slider_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || g_volume_slider == NULL) {
        return;
    }
    if(g_settings_volume_syncing != 0u) {
        return;
    }

    int32_t volume = lv_slider_get_value(g_volume_slider) * 10;
    g_settings_volume = volume;
    volume_overlay_refresh_hide_timer();
    if(g_callbacks.settings_volume_changed != NULL) {
        g_callbacks.settings_volume_changed(volume);
    }
}

static void volume_overlay_create(void)
{
    if(g_volume_overlay != NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if(screen == NULL) {
        return;
    }

    g_volume_overlay = lv_obj_create(screen);
    lv_obj_remove_flag(g_volume_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_volume_overlay, 34, 170);
    lv_obj_align(g_volume_overlay, LV_ALIGN_RIGHT_MID, -6, 2);
    lv_obj_set_style_radius(g_volume_overlay, 6, 0);
    lv_obj_set_style_bg_color(g_volume_overlay, lv_color_make(0x16, 0x16, 0x16), 0);
    lv_obj_set_style_bg_opa(g_volume_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(g_volume_overlay, 1, 0);
    lv_obj_set_style_border_color(g_volume_overlay, lv_color_make(0x58, 0x58, 0x58), 0);
    lv_obj_set_style_pad_all(g_volume_overlay, 0, 0);

    lv_obj_t *icon = lv_label_create(g_volume_overlay);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, ui_font_small(), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);

    g_volume_slider = lv_slider_create(g_volume_overlay);
    lv_slider_set_range(g_volume_slider, 0, 10);
    lv_obj_set_size(g_volume_slider, 14, 120);
    lv_obj_align(g_volume_slider, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_make(0x78, 0x78, 0x78), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_volume_slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_volume_slider, UI_COLOR_SETTINGS, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_volume_slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_volume_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(g_volume_slider, volume_overlay_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(g_volume_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ==========================================================================
 * 设置页面事件处理
 * ========================================================================== */

static void settings_set_network_selected(ui_settings_view_t *view, ui_settings_network_mode_t mode)
{
    if(view == NULL) {
        return;
    }

    if(g_settings_pending_network != UI_SETTINGS_NETWORK_NONE &&
       mode == g_settings_pending_network) {
        g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
    }

    view->selected_network = mode;
    lv_color_t active_bg = lv_color_white();
    lv_color_t active_text = lv_color_black();
    lv_color_t pending_bg = lv_color_make(0x22, 0x22, 0x22);
    lv_color_t pending_text = lv_color_make(0xA8, 0xA8, 0xA8);
    lv_color_t idle_bg = lv_color_black();
    lv_color_t idle_text = lv_color_white();
    if(view->wlan_button != NULL) {
        bool active = mode == UI_SETTINGS_NETWORK_WLAN;
        bool pending = g_settings_pending_network == UI_SETTINGS_NETWORK_WLAN && !active;
        lv_obj_set_style_bg_color(view->wlan_button, active ? active_bg : (pending ? pending_bg : idle_bg), 0);
        lv_obj_set_style_border_width(view->wlan_button, pending ? 1 : 0, 0);
        lv_obj_set_style_border_color(view->wlan_button, lv_color_make(0x58, 0x58, 0x58), 0);
        if(view->wlan_ssid_label != NULL) {
            lv_obj_set_style_text_color(view->wlan_ssid_label, active ? active_text : (pending ? pending_text : idle_text), 0);
        }
    }
    if(view->cellular_button != NULL) {
        bool active = mode == UI_SETTINGS_NETWORK_4G;
        bool pending = g_settings_pending_network == UI_SETTINGS_NETWORK_4G && !active;
        lv_obj_set_style_bg_color(view->cellular_button, active ? active_bg : (pending ? pending_bg : idle_bg), 0);
        lv_obj_set_style_border_width(view->cellular_button, pending ? 1 : 0, 0);
        lv_obj_set_style_border_color(view->cellular_button, lv_color_make(0x58, 0x58, 0x58), 0);
        if(view->cellular_label != NULL) {
            lv_label_set_text(view->cellular_label, "移动数据");
            lv_obj_set_style_text_color(view->cellular_label, active ? active_text : (pending ? pending_text : idle_text), 0);
        }
    }
}

static void settings_refresh_wlan_ssid(ui_settings_view_t *view)
{
    char ssid[33];
    lv_point_t text_size;
    const char *current_text;

    if(view == NULL || view->wlan_ssid_label == NULL) {
        return;
    }

    ssid[0] = '\0';
    current_text = lv_label_get_text(view->wlan_ssid_label);
    if(view->selected_network == UI_SETTINGS_NETWORK_WLAN &&
       g_callbacks.settings_wifi_ssid_get != NULL &&
       g_callbacks.settings_wifi_ssid_get(ssid, sizeof(ssid)) == 0 &&
       ssid[0] != '\0') {
        if(current_text == NULL || strcmp(current_text, ssid) != 0) {
            lv_label_set_text(view->wlan_ssid_label, ssid);
        }
        const lv_font_t *font = lv_obj_get_style_text_font(view->wlan_ssid_label, LV_PART_MAIN);
        int32_t letter_space = lv_obj_get_style_text_letter_space(view->wlan_ssid_label, LV_PART_MAIN);
        int32_t line_space = lv_obj_get_style_text_line_space(view->wlan_ssid_label, LV_PART_MAIN);
        lv_text_get_size(&text_size, ssid, font, letter_space, line_space, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        lv_label_set_long_mode(view->wlan_ssid_label,
                               text_size.x > lv_obj_get_width(view->wlan_ssid_label)
                                   ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                   : LV_LABEL_LONG_CLIP);
    } else {
        if(current_text == NULL || strcmp(current_text, "WLAN") != 0) {
            lv_label_set_text(view->wlan_ssid_label, "WLAN");
        }
        lv_label_set_long_mode(view->wlan_ssid_label, LV_LABEL_LONG_CLIP);
    }
}

static const char *settings_4g_status_text(ui_settings_4g_status_t status)
{
    switch(status) {
        case UI_SETTINGS_4G_NO_SIM:
            return "未插卡";
        case UI_SETTINGS_4G_NO_AT:
            return "4G模块无响应";
        case UI_SETTINGS_4G_NOT_REGISTERED:
            return "4G未注册网络";
        case UI_SETTINGS_4G_UNAVAILABLE:
            return "4G不可用";
        default:
            return "4G已启用";
    }
}

static void settings_refresh_network_selected(ui_settings_view_t *view)
{
    if(view == NULL) {
        return;
    }

    if(g_callbacks.settings_network_mode_get != NULL) {
        settings_set_network_selected(view, g_callbacks.settings_network_mode_get());
        return;
    }

    settings_set_network_selected(view,
                                  view->selected_network == UI_SETTINGS_NETWORK_NONE
                                      ? UI_SETTINGS_NETWORK_WLAN
                                      : view->selected_network);
}

static void settings_refresh_timer_cb(lv_timer_t *timer)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_timer_get_user_data(timer);
    if(view == NULL || view != g_settings_view) {
        return;
    }

    settings_refresh_network_selected(view);
    settings_refresh_wlan_ssid(view);
}

static void settings_wlan_back_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL || view->wlan_page == NULL) {
        return;
    }

    lv_obj_add_flag(view->wlan_page, LV_OBJ_FLAG_HIDDEN);
    settings_refresh_network_selected(view);
    settings_refresh_wlan_ssid(view);
}

static void settings_wifi_ap_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = g_settings_view;
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL || ssid == NULL) {
        return;
    }

    strncpy(view->selected_ssid, ssid, sizeof(view->selected_ssid) - 1u);
    view->selected_ssid[sizeof(view->selected_ssid) - 1u] = '\0';
    if(view->password_dialog != NULL) {
        lv_obj_align(view->password_dialog, LV_ALIGN_TOP_MID, 0, 26);
        lv_obj_move_foreground(view->password_dialog);
        lv_obj_remove_flag(view->password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->password_title_label != NULL) {
        lv_label_set_text_fmt(view->password_title_label, "%s", view->selected_ssid);
        lv_obj_set_pos(view->password_title_label, 0, 0);
        lv_obj_set_width(view->password_title_label, 188);
    }
    if(view->password_textarea != NULL) {
        lv_textarea_set_text(view->password_textarea, "");
        lv_obj_remove_flag(view->password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(view->password_textarea, LV_STATE_FOCUSED);
    }
    if(view->password_keyboard != NULL) {
        lv_keyboard_set_textarea(view->password_keyboard, view->password_textarea);
        lv_obj_move_foreground(view->password_keyboard);
        lv_obj_remove_flag(view->password_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connect_button != NULL) {
        lv_obj_remove_flag(view->connect_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->cancel_button != NULL) {
        lv_obj_remove_flag(view->cancel_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connecting_spinner != NULL) {
        lv_obj_add_flag(view->connecting_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_hide_wifi_dialog(ui_settings_view_t *view)
{
    if(view == NULL) {
        return;
    }
    view->selected_ssid[0] = '\0';
    if(view->password_dialog != NULL) {
        lv_obj_add_flag(view->password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->password_textarea != NULL) {
        lv_obj_add_flag(view->password_textarea, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->password_keyboard != NULL) {
        lv_obj_add_flag(view->password_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connect_button != NULL) {
        lv_obj_add_flag(view->connect_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->cancel_button != NULL) {
        lv_obj_add_flag(view->cancel_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connecting_spinner != NULL) {
        lv_obj_add_flag(view->connecting_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_wifi_clear_list(ui_settings_view_t *view)
{
    if(view == NULL || view->wlan_list == NULL) {
        return;
    }

    lv_obj_clean(view->wlan_list);
}

static const char *settings_wifi_signal_text(int rssi)
{
    if(rssi >= -55) {
        return "优";
    }
    if(rssi >= -67) {
        return "良";
    }
    if(rssi >= -78) {
        return "中";
    }
    return "差";
}

static void settings_wifi_add_ap(ui_settings_view_t *view, const ui_settings_wifi_ap_t *ap)
{
    if(view == NULL || view->wlan_list == NULL || ap == NULL || ap->ssid[0] == '\0') {
        return;
    }

    lv_obj_t *btn = lv_button_create(view->wlan_list);
    lv_obj_set_size(btn, 196, 34);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_make(0x55, 0x55, 0x55), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *ssid = lv_label_create(btn);
    lv_label_set_text(ssid, ap->ssid);
    lv_obj_set_pos(ssid, 10, 9);
    lv_obj_set_width(ssid, 142);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(ssid, lv_color_white(), 0);

    lv_obj_t *signal = lv_label_create(btn);
    lv_label_set_text(signal, settings_wifi_signal_text(ap->rssi));
    lv_obj_set_pos(signal, 158, 9);
    lv_obj_set_width(signal, 26);
    lv_obj_set_style_text_align(signal, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(signal, lv_color_make(0xF0, 0xF0, 0xF0), 0);

    lv_obj_add_event_cb(btn, settings_wifi_ap_event_cb, LV_EVENT_CLICKED, (void *)lv_label_get_text(ssid));
}

static void settings_wifi_scan(ui_settings_view_t *view)
{
    if(view == NULL || g_callbacks.settings_wifi_scan_requested == NULL) {
        return;
    }

    settings_wifi_clear_list(view);
    settings_hide_wifi_dialog(view);
    if(view->wlan_status_label != NULL) {
        lv_label_set_text(view->wlan_status_label, "正在扫描...");
    }
    g_callbacks.settings_wifi_scan_requested();
}

static void settings_wlan_scan_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_wifi_scan((ui_settings_view_t *)lv_event_get_user_data(e));
    }
}

static void settings_wifi_cancel_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_hide_wifi_dialog((ui_settings_view_t *)lv_event_get_user_data(e));
    }
}

static void settings_wifi_connect_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL ||
       g_callbacks.settings_wifi_connect_requested == NULL ||
       view->selected_ssid[0] == '\0') {
        return;
    }

    const char *password = view->password_textarea != NULL ? lv_textarea_get_text(view->password_textarea) : "";
    if(password == NULL || strlen(password) < 8u) {
        if(view->password_title_label != NULL) {
            lv_label_set_text(view->password_title_label, "密码至少8位");
            lv_obj_set_pos(view->password_title_label, 0, 0);
            lv_obj_set_width(view->password_title_label, 188);
        }
        if(view->password_textarea != NULL) {
            lv_obj_remove_flag(view->password_textarea, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(view->password_textarea, LV_STATE_FOCUSED);
        }
        if(view->password_keyboard != NULL) {
            lv_obj_remove_flag(view->password_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if(view->password_dialog != NULL) {
        lv_obj_align(view->password_dialog, LV_ALIGN_CENTER, 0, -6);
    }
    if(view->password_title_label != NULL) {
        lv_label_set_text(view->password_title_label, "正在连接...");
        lv_obj_align(view->password_title_label, LV_ALIGN_TOP_MID, 0, 6);
    }
    if(view->password_textarea != NULL) {
        lv_obj_add_flag(view->password_textarea, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->password_keyboard != NULL) {
        lv_obj_add_flag(view->password_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connect_button != NULL) {
        lv_obj_add_flag(view->connect_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->cancel_button != NULL) {
        lv_obj_add_flag(view->cancel_button, LV_OBJ_FLAG_HIDDEN);
    }
    if(view->connecting_spinner != NULL) {
        lv_obj_remove_flag(view->connecting_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    g_callbacks.settings_wifi_connect_requested(view->selected_ssid, password);
}

static void settings_4g_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || view == NULL ||
       g_callbacks.settings_4g_select_requested == NULL) {
        return;
    }
    if(view->selected_network == UI_SETTINGS_NETWORK_4G) {
        return;
    }
    if(g_settings_pending_network == UI_SETTINGS_NETWORK_4G) {
        settings_refresh_network_selected(view);
        return;
    }

    int ret = g_callbacks.settings_4g_select_requested();
    if(ret == 0) {
        g_settings_pending_network = UI_SETTINGS_NETWORK_4G;
        settings_refresh_network_selected(view);
        settings_refresh_wlan_ssid(view);
    } else if(g_settings_pending_network == UI_SETTINGS_NETWORK_NONE) {
        g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
        settings_refresh_network_selected(view);
    }
}

static void settings_wlan_select_event_cb(lv_event_t *e)
{
    ui_settings_view_t *view = (ui_settings_view_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if(view == NULL || view->wlan_page == NULL) {
        return;
    }

    if(code == LV_EVENT_CLICKED) {
        if(view->selected_network == UI_SETTINGS_NETWORK_WLAN) {
            return;
        }
        if(g_settings_pending_network == UI_SETTINGS_NETWORK_WLAN) {
            settings_refresh_network_selected(view);
            return;
        }
        if(view->network_status_label != NULL) {
            lv_obj_remove_flag(view->network_status_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(view->network_status_label, "正在切换WLAN...");
        }
        if(g_callbacks.settings_wifi_select_requested != NULL) {
            g_settings_pending_network = UI_SETTINGS_NETWORK_WLAN;
            settings_refresh_network_selected(view);
            g_callbacks.settings_wifi_select_requested();
        } else {
            lv_obj_remove_flag(view->wlan_page, LV_OBJ_FLAG_HIDDEN);
            settings_wifi_scan(view);
        }
    } else if(code == LV_EVENT_LONG_PRESSED) {
        lv_obj_remove_flag(view->wlan_page, LV_OBJ_FLAG_HIDDEN);
        settings_wifi_scan(view);
    }
}

/* ==========================================================================
 * 公开接口
 * ========================================================================== */

void ui_event_set_callbacks(const ui_event_callbacks_t *callbacks)
{
    if(callbacks == NULL) {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
        return;
    }

    g_callbacks = *callbacks;
}

void ui_event_notify_power_shutdown_confirmed(void)
{
    if(g_callbacks.power_shutdown_confirmed != NULL) {
        g_callbacks.power_shutdown_confirmed();
    }
}

/**
 * @brief 注册对讲页面的 LVGL 控件事件。
 *
 * 绑定：
 * - channel_dec_button → intercom_dec_event_cb（LV_EVENT_ALL）
 * - channel_inc_button → intercom_inc_event_cb（LV_EVENT_ALL）
 * - ptt_button → intercom_ptt_event_cb（LV_EVENT_ALL）
 *
 * 全事件类型注册（LV_EVENT_ALL）是为了同时捕获
 * CLICKED、LONG_PRESSED、LONG_PRESSED_REPEAT、RELEASED 等。
 */
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
}

void ui_event_notify_camera_entered(void)
{
    if(g_callbacks.camera_entered != NULL) {
        g_callbacks.camera_entered();
    }
}

void ui_event_notify_camera_exited(void)
{
    if(g_callbacks.camera_exited != NULL) {
        g_callbacks.camera_exited();
    }
}

void ui_event_register_ai(ui_ai_view_t *view)
{
    if(view == NULL || view->ask_button == NULL) {
        return;
    }

    g_ai_view = view;
    if(g_ai_waiting == 0u) {
        g_ai_message_id = UI_TEXT_AI_IDLE;
        g_ai_audio_btn_state = UI_AI_AUDIO_BTN_HIDDEN;
    }
    ai_set_speaking(view, false);
    if(view->camera_button != NULL) {
        lv_obj_add_event_cb(view->camera_button, ai_camera_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->audio_button != NULL) {
        lv_obj_add_event_cb(view->audio_button, ai_audio_play_event_cb, LV_EVENT_CLICKED, view);
    }
    lv_obj_add_event_cb(view->ask_button, ai_ask_event_cb, LV_EVENT_ALL, view);
    ai_apply_audio_button_state(view, g_ai_audio_btn_state);
    if(view->answer_label != NULL) {
        lv_label_set_text(view->answer_label, ui_i18n_text(g_ai_message_id));
        lv_obj_set_style_text_font(view->answer_label, ui_font_normal(), 0);
        ai_answer_scroll_top(view);
    }
    if(g_ai_waiting != 0u) {
        ui_event_set_ai_waiting(true);
    }
}

void ui_event_unregister_ai(ui_ai_view_t *view)
{
    if(g_ai_view != view) {
        return;
    }

    if(g_ai_wait_timer != NULL) {
        lv_timer_delete(g_ai_wait_timer);
        g_ai_wait_timer = NULL;
    }
    g_ai_view = NULL;
    g_ai_cancel_label = NULL;
}

void ui_event_set_ai_waiting(bool waiting)
{
    g_ai_waiting = waiting ? 1u : 0u;
    ai_apply_cancel_entry(g_ai_view);

    if(!waiting) {
        if(g_ai_wait_timer != NULL) {
            lv_timer_delete(g_ai_wait_timer);
            g_ai_wait_timer = NULL;
        }
        g_ai_wait_dot_count = 0u;
        if(g_ai_view != NULL && g_ai_view->answer_label != NULL) {
            lv_label_set_text(g_ai_view->answer_label, ui_i18n_text(g_ai_message_id));
            ai_answer_scroll_top(g_ai_view);
        }
        return;
    }

    g_ai_wait_dot_count = 0u;
    if(g_ai_view != NULL && g_ai_view->answer_label != NULL) {
        lv_label_set_text(g_ai_view->answer_label, ui_i18n_text(g_ai_message_id));
        lv_obj_set_style_text_font(g_ai_view->answer_label, ui_font_normal(), 0);
        ai_answer_scroll_top(g_ai_view);
    }
    if(g_ai_wait_timer == NULL) {
        g_ai_wait_timer = lv_timer_create(ai_wait_timer_cb, 320, NULL);
    }
}

void ui_event_set_ai_message(ui_text_id_t text_id)
{
    g_ai_message_id = text_id;
    ui_event_set_ai_waiting(false);
    if(g_ai_view != NULL && g_ai_view->answer_label != NULL) {
        lv_label_set_text(g_ai_view->answer_label, ui_i18n_text(text_id));
        lv_obj_set_style_text_font(g_ai_view->answer_label, ui_font_normal(), 0);
        ai_answer_scroll_top(g_ai_view);
    }
}

void ui_event_set_ai_answer_text(const char *text)
{
    ui_event_set_ai_waiting(false);
    if(g_ai_view != NULL && g_ai_view->answer_label != NULL) {
        lv_label_set_text(g_ai_view->answer_label, text != NULL ? text : "");
        lv_obj_set_style_text_font(g_ai_view->answer_label, ui_font_normal(), 0);
        ai_answer_scroll_top(g_ai_view);
    }
}

void ui_event_set_ai_audio_button_state(ui_ai_audio_btn_state_t state)
{
    g_ai_audio_btn_state = state;
    ai_apply_audio_button_state(g_ai_view, state);
}

void ui_event_set_settings_volume(int32_t volume)
{
    volume = normalize_volume_step(volume);

    g_settings_volume = volume;
    volume_overlay_create();
    if(g_volume_overlay == NULL || g_volume_slider == NULL) {
        return;
    }

    g_settings_volume_syncing = 1u;
    lv_slider_set_value(g_volume_slider, volume / 10, LV_ANIM_OFF);
    g_settings_volume_syncing = 0u;
    lv_obj_remove_flag(g_volume_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_volume_overlay);
    volume_overlay_refresh_hide_timer();
}

void ui_event_set_settings_brightness(int32_t brightness)
{
    brightness = normalize_percent(brightness);

    g_settings_brightness = brightness;
    ui_settings_view_t *view = g_settings_view;
    if(view == NULL || view->brightness_slider == NULL) {
        return;
    }

    g_settings_brightness_syncing = 1u;
    lv_slider_set_value(view->brightness_slider, brightness, LV_ANIM_OFF);
    g_settings_brightness_syncing = 0u;
    settings_refresh_brightness_label(view);
}

void ui_event_register_settings(ui_settings_view_t *view)
{
    if(view == NULL) {
        return;
    }

    g_settings_view = view;
    if(view->wlan_button != NULL) {
        lv_obj_add_event_cb(view->wlan_button, settings_wlan_select_event_cb, LV_EVENT_ALL, view);
    }
    if(view->cellular_button != NULL) {
        lv_obj_add_event_cb(view->cellular_button, settings_4g_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->wlan_back_button != NULL) {
        lv_obj_add_event_cb(view->wlan_back_button, settings_wlan_back_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->wlan_scan_button != NULL) {
        lv_obj_add_event_cb(view->wlan_scan_button, settings_wlan_scan_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->connect_button != NULL) {
        lv_obj_add_event_cb(view->connect_button, settings_wifi_connect_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->cancel_button != NULL) {
        lv_obj_add_event_cb(view->cancel_button, settings_wifi_cancel_event_cb, LV_EVENT_CLICKED, view);
    }
    if(view->brightness_slider != NULL) {
        lv_obj_add_event_cb(view->brightness_slider, settings_brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, view);
    }
    settings_refresh_network_selected(view);
    settings_refresh_wlan_ssid(view);
    ui_event_set_settings_brightness(g_settings_brightness);
    if(g_settings_refresh_timer == NULL) {
        g_settings_refresh_timer = lv_timer_create(settings_refresh_timer_cb, 1000, view);
    } else {
        lv_timer_set_user_data(g_settings_refresh_timer, view);
        lv_timer_resume(g_settings_refresh_timer);
    }
}

void ui_event_unregister_settings(ui_settings_view_t *view)
{
    if(g_settings_view == view) {
        g_settings_view = NULL;
    }
    if(g_settings_refresh_timer != NULL) {
        lv_timer_delete(g_settings_refresh_timer);
        g_settings_refresh_timer = NULL;
    }
}

void ui_event_settings_show_wlan_scan_result(const ui_settings_wifi_ap_t *items,
                                             uint16_t count,
                                             int ret)
{
    ui_settings_view_t *view = g_settings_view;
    if(view == NULL) {
        return;
    }

    settings_wifi_clear_list(view);
    if(ret != 0) {
        if(view->wlan_status_label != NULL) {
            lv_label_set_text(view->wlan_status_label, "扫描失败");
        }
        return;
    }

    for(uint16_t i = 0; i < count; i++) {
        settings_wifi_add_ap(view, &items[i]);
    }
    if(view->wlan_status_label != NULL) {
        lv_label_set_text(view->wlan_status_label, count > 0 ? "选择热点" : "未发现热点");
    }
}

void ui_event_settings_show_wifi_connect_result(int ret)
{
    ui_settings_view_t *view = g_settings_view;
    if(view == NULL) {
        return;
    }

    if(ret == -2) {
        if(view->password_title_label != NULL) {
            lv_label_set_text(view->password_title_label, "密码至少8位");
            lv_obj_set_pos(view->password_title_label, 0, 0);
            lv_obj_set_width(view->password_title_label, 188);
        }
        if(view->password_dialog != NULL) {
            lv_obj_remove_flag(view->password_dialog, LV_OBJ_FLAG_HIDDEN);
        }
        if(view->password_textarea != NULL) {
            lv_obj_remove_flag(view->password_textarea, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(view->password_textarea, LV_STATE_FOCUSED);
        }
        if(view->password_keyboard != NULL) {
            lv_obj_remove_flag(view->password_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if(ret == 0) {
        g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
        if(view->wlan_status_label != NULL) {
            lv_label_set_text(view->wlan_status_label, "WLAN已启用");
        }
        if(view->network_status_label != NULL) {
            lv_label_set_text(view->network_status_label, "WLAN已启用");
        }
        settings_set_network_selected(view, UI_SETTINGS_NETWORK_WLAN);
        settings_refresh_wlan_ssid(view);
        settings_hide_wifi_dialog(view);
        if(view->wlan_page != NULL) {
            lv_obj_add_flag(view->wlan_page, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    settings_hide_wifi_dialog(view);
    g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
    if(view->wlan_status_label != NULL) {
        lv_label_set_text(view->wlan_status_label, "连接失败");
    }
    if(view->network_status_label != NULL) {
        lv_label_set_text(view->network_status_label, "WLAN连接失败");
    }
    settings_refresh_network_selected(view);
}

void ui_event_settings_show_4g_select_result(ui_settings_4g_status_t status, int ret)
{
    ui_settings_view_t *view = g_settings_view;
    if(view == NULL) {
        return;
    }

    if(view->network_status_label != NULL) {
        lv_label_set_text(view->network_status_label, settings_4g_status_text(status));
    }
    if(ret == 0) {
        g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
        settings_set_network_selected(view, UI_SETTINGS_NETWORK_4G);
    } else {
        g_settings_pending_network = UI_SETTINGS_NETWORK_NONE;
        settings_refresh_network_selected(view);
    }
    settings_refresh_wlan_ssid(view);
}
