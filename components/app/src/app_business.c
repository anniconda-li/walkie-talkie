/**
 * @file app_business.c
 * @brief 第一版业务启动编排、UI 事件转发和音频会话互斥。
 *
 * ## 模块职责
 * - **启动编排**：按顺序启动 UI、状态监控、对讲、AI 问答等后台模块
 * - **事件转发**：将 UI 层回调注册到各 app 模块（PTT→app_intercom, AI→app_ai_voice）
 * - **音频会话互斥**：PTT 和 AI 问答共享同一个音频硬件，通过互斥锁 + 标志位保证
 *   同一时刻只有一个业务占用麦克风和扬声器
 *
 * ## 音频会话互斥机制
 * ```
 *   PTT 长按                          AI 长按（同时发生）
 *      │                                  │
 *      ├─ try_begin() → busy=1 ✓         ├─ try_begin() → busy 已为 1 ✗
 *      │   └─ 开始录音+发送               │   └─ return（被忽略）
 *      │                                  │
 *      └─ 松开 → session_end() → busy=0   └─ （第二次按→try_begin 成功）
 * ```
 *
 * try_begin() 使用非阻塞 mutex（OSAL_WAIT_NONE），如果拿不到锁立即返回失败，
 * 确保 UI 长按事件处理函数不会被阻塞。
 *
 * ## 关键设计决策
 * - mutex 只保护 s_audio_session_busy 的读写原子性，不用于阻塞等待
 * - try_begin 失败不重试——对讲机场景下"抢占即失败"是预期行为
 * - 每个业务模块独立管理自己的后台任务，app_business 只做编排和转发
 */
#include "app_business.h"

#include "app_ai_voice.h"
#include "app_camera.h"
#include "app_config.h"
#include "app_intercom.h"
#include "app_network.h"
#include "app_status_monitor.h"
#include "app_ui.h"
#include "d_power_control.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_buttons.h"
#include "service_screen.h"
#include "ui_event.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_business";

#define APP_BUSINESS_VOLUME_TASK_STACK 3072u
#define APP_BUSINESS_VOLUME_QUEUE_LEN  8u

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief 业务启动是否已完成（防重复初始化）。 */
static volatile int s_started = 0;

/**
 * @brief 音频会话互斥锁。
 *
 * 保护 s_audio_session_busy 的读写原子性。
 * 底层由 OSAL 适配到当前系统互斥量实现。
 */
static osal_mutex_t s_audio_session_mutex = NULL;

/**
 * @brief 音频会话占用标志。
 *
 * 1 = 已被 PTT 或 AI 占用，其他人不可抢占；
 * 0 = 空闲，可抢占。
 *
 * 必须在持锁状态下读写，保证多任务环境下的原子性。
 */
static int s_audio_session_busy = 0;
static volatile int s_wifi_scan_busy = 0;
static volatile int s_wifi_connect_busy = 0;
static volatile int s_network_switch_busy = 0;
static volatile int s_fake_4g_connect_busy = 0;
static int32_t s_volume = 80;
static osal_queue_t s_volume_step_queue = NULL;
static osal_task_t s_volume_task = NULL;

typedef struct {
    char ssid[33];
    char password[65];
} app_business_wifi_connect_req_t;

/* ==========================================================================
 * 音频会话管理
 * ========================================================================== */

/**
 * @brief 惰性创建音频会话互斥锁。
 *
 * 首次调用 try_begin() 或 is_busy() 时自动创建，
 * 避免在 app_business_start() 之前被调用导致空指针。
 *
 * @return 成功返回 0；创建失败返回 -1。
 */
static int app_business_audio_session_init(void)
{
    if (s_audio_session_mutex != NULL) {
        return 0;
    }

    s_audio_session_mutex = osal_mutex_create();
    return s_audio_session_mutex == NULL ? -1 : 0;
}

/**
 * @brief 尝试占用音频会话（非阻塞）。
 *
 * ## 执行流程
 * 1. 确保互斥锁已创建
 * 2. 非阻塞尝试获取互斥锁（OSAL_WAIT_NONE = 0 tick）
 * 3. 获取互斥锁后检查 s_audio_session_busy 标志
 * 4. 若空闲，设置 s_audio_session_busy = 1，返回成功
 * 5. 若已占用，释放锁，返回 -3
 *
 * ## 为什么不用 mutex 本身做互斥？
 * 因为"获取成功后再释放"和"获取失败时查状态"是两种不同的语义。
 * 单独的 busy 标志配合 mutex 可以实现非阻塞查询和尝试占用，
 * 而不需要依赖底层 mutex 的递归/超时特性。
 *
 * @return 0=成功占用；-1=锁未初始化；-2=拿不到锁；-3=已被占用。
 */
int app_business_audio_session_try_begin(void)
{
    if (app_business_audio_session_init() != 0) {
        return -1;
    }

    /* 非阻塞获取：业务层只做抢占失败即放弃，避免 UI 长按事件被卡住。 */
    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_NONE) != 0) {
        return -2;
    }

    if (s_audio_session_busy) {
        osal_mutex_unlock(s_audio_session_mutex);
        return -3;
    }

    s_audio_session_busy = 1;
    osal_mutex_unlock(s_audio_session_mutex);
    return 0;
}

/**
 * @brief 释放音频会话占用。
 *
 * 由 PTT 停止或 AI 问答完成后调用。
 * 将 s_audio_session_busy 清零，使其他业务可以抢占。
 *
 * 使用 OSAL_WAIT_FOREVER 等待锁——释放操作不应失败。
 */
void app_business_audio_session_end(void)
{
    if (s_audio_session_mutex == NULL) {
        return;
    }

    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_FOREVER) == 0) {
        s_audio_session_busy = 0;
        osal_mutex_unlock(s_audio_session_mutex);
    }
}

/**
 * @brief 查询音频会话是否正被占用（非阻塞）。
 *
 * 用于 PTT/AI 在开始录音前快速判断是否可抢占，
 * 不影响 s_audio_session_busy 的值。
 *
 * @return 1=已占用；0=空闲。
 */
int app_business_audio_session_is_busy(void)
{
    int busy = 0;

    if (s_audio_session_mutex == NULL) {
        return 0;
    }

    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_NONE) == 0) {
        busy = s_audio_session_busy;
        osal_mutex_unlock(s_audio_session_mutex);
    }

    return busy;
}

/* ==========================================================================
 * UI 事件回调转发
 *
 * 这些函数是 UI 层和业务模块之间的桥接层。
 * UI 层（ui_event.c）在用户操作时调用回调函数指针，
 * app_business 将其注册为具体的业务函数。
 * ========================================================================== */

/** @brief 频道切换 → 通知对讲模块更新频道号。 */
static void app_business_on_channel_changed(int32_t channel)
{
    app_intercom_set_channel(channel);
}

/** @brief PTT 按下 → 启动对讲发送流程。 */
static void app_business_on_ptt_started(int32_t channel)
{
    app_intercom_ptt_start(channel);
}

/** @brief PTT 松开 → 停止对讲发送。 */
static void app_business_on_ptt_stopped(int32_t channel)
{
    (void)channel;
    app_intercom_ptt_stop();
}

/** @brief AI 按钮按下 → 开始 AI 录音。 */
static void app_business_on_ai_started(void)
{
    app_ai_voice_record_start();
}

/** @brief AI 按钮松开 → 停止录音并触发上传+播放流程。 */
static void app_business_on_ai_stopped(void)
{
    app_ai_voice_record_stop();
}

static void app_business_on_ai_reply_play_requested(void)
{
    app_ai_voice_request_reply_play();
}

static void app_business_on_ai_reply_stop_requested(void)
{
    app_ai_voice_request_reply_stop();
}

static void app_business_on_ai_cancel_requested(void)
{
    if (app_camera_cancel_current() == 0) {
        return;
    }
    (void)app_ai_voice_cancel_current();
}

/** @brief 相机页进入 → 启动预览。 */
static void app_business_on_camera_entered(void)
{
    app_camera_enter();
}

/** @brief 相机页退出 → 停止预览并清理暂存图像。 */
static void app_business_on_camera_exited(void)
{
    app_camera_exit();
}

/** @brief 相机拍照按钮 → 后台拍 JPEG 并暂存。 */
static void app_business_on_camera_capture(void)
{
    app_camera_capture();
}

/** @brief 相机上传按钮 → HTTP POST 上传暂存 JPEG。 */
static int app_business_on_camera_upload(void)
{
    return app_camera_upload();
}

/** @brief 相机重拍按钮 → 清理 JPEG 并恢复预览。 */
static void app_business_on_camera_retake(void)
{
    app_camera_retake();
}

static void app_business_set_volume(int32_t value, int sync_ui)
{
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    value = ((value + 5) / 10) * 10;

    if (value == s_volume) {
        if (sync_ui) {
            (void)app_ui_set_settings_volume(value);
        }
        return;
    }

    s_volume = value;
    (void)service_audio_set_volume((uint8_t)value);
    if (sync_ui) {
        (void)app_ui_set_settings_volume(value);
    }
    APP_LOGI(TAG, "音量已设置, volume=%ld", (long)value);
}

/** @brief 音量滑块变化 → 设置硬件音量（0-100）。 */
static void app_business_on_volume_changed(int32_t value)
{
    app_business_set_volume(value, 0);
}

static void app_business_on_volume_button_step(int step)
{
    if (s_volume_step_queue != NULL) {
        (void)osal_queue_send(s_volume_step_queue, &step, OSAL_WAIT_NONE);
    }
}

static void app_business_volume_task(void *arg)
{
    (void)arg;
    int step = 0;

    while (1) {
        if (osal_queue_recv(s_volume_step_queue, &step, OSAL_WAIT_FOREVER) == 0) {
            app_business_set_volume(s_volume + step, 1);
        }
    }
}

static int app_business_start_volume_task(void)
{
    if (s_volume_step_queue == NULL) {
        s_volume_step_queue = osal_queue_create(APP_BUSINESS_VOLUME_QUEUE_LEN, sizeof(int));
        if (s_volume_step_queue == NULL) {
            return -1;
        }
    }
    if (s_volume_task != NULL) {
        return 0;
    }

    return osal_task_create("biz_volume",
                            app_business_volume_task,
                            NULL,
                            APP_BUSINESS_VOLUME_TASK_STACK,
                            3u,
                            &s_volume_task);
}

static ui_settings_network_mode_t app_business_get_network_mode(void)
{
    switch (app_network_get_mode()) {
        case APP_NETWORK_MODE_WIFI:
            return UI_SETTINGS_NETWORK_WLAN;
        case APP_NETWORK_MODE_4G:
            return UI_SETTINGS_NETWORK_4G;
        default:
            return UI_SETTINGS_NETWORK_NONE;
    }
}

static int app_business_get_wifi_ssid(char *ssid, size_t size)
{
    return app_network_get_wifi_ssid(ssid, size);
}

static void app_business_wifi_scan_task(void *arg)
{
    (void)arg;

    app_network_wifi_ap_t aps[UI_SETTINGS_WIFI_AP_MAX];
    size_t app_count = 0u;
    ui_settings_wifi_ap_t ui_aps[UI_SETTINGS_WIFI_AP_MAX];
    int ret = app_network_scan_wifi(aps, UI_SETTINGS_WIFI_AP_MAX, &app_count);

    if (ret == 0) {
        for (size_t i = 0; i < app_count && i < UI_SETTINGS_WIFI_AP_MAX; i++) {
            strncpy(ui_aps[i].ssid, aps[i].ssid, sizeof(ui_aps[i].ssid) - 1u);
            ui_aps[i].ssid[sizeof(ui_aps[i].ssid) - 1u] = '\0';
            ui_aps[i].rssi = aps[i].rssi;
        }
    } else {
        app_count = 0u;
    }

    (void)app_ui_settings_show_wlan_scan_result(ui_aps, (uint16_t)app_count, ret);
    s_wifi_scan_busy = 0;
    osal_task_delete_current();
}

static void app_business_on_wifi_scan(void)
{
    if (s_wifi_scan_busy) {
        return;
    }
    s_wifi_scan_busy = 1;
    if (osal_task_create("wifi_scan",
                         app_business_wifi_scan_task,
                         NULL,
                         6144u,
                         4u,
                         NULL) != 0) {
        s_wifi_scan_busy = 0;
        (void)app_ui_settings_show_wlan_scan_result(NULL, 0u, -1);
    }
}

static void app_business_wifi_connect_task(void *arg)
{
    app_business_wifi_connect_req_t *req = (app_business_wifi_connect_req_t *)arg;
    int ret = -1;

    if (req != NULL) {
        ret = app_network_connect_wifi(req->ssid, req->password);
        free(req);
    }

    (void)app_ui_settings_show_wifi_connect_result(ret);
    s_wifi_connect_busy = 0;
    s_network_switch_busy = 0;
    osal_task_delete_current();
}

static void app_business_on_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || s_wifi_connect_busy || s_network_switch_busy) {
        return;
    }
    if (password == NULL || strlen(password) < 8u) {
        (void)app_ui_settings_show_wifi_connect_result(-2);
        return;
    }

    app_business_wifi_connect_req_t *req = malloc(sizeof(*req));
    if (req == NULL) {
        (void)app_ui_settings_show_wifi_connect_result(-1);
        return;
    }
    memset(req, 0, sizeof(*req));
    strncpy(req->ssid, ssid, sizeof(req->ssid) - 1u);
    strncpy(req->password, password != NULL ? password : "", sizeof(req->password) - 1u);

    s_wifi_connect_busy = 1;
    s_network_switch_busy = 1;
    if (osal_task_create("wifi_conn",
                         app_business_wifi_connect_task,
                         req,
                         6144u,
                         4u,
                         NULL) != 0) {
        s_wifi_connect_busy = 0;
        s_network_switch_busy = 0;
        free(req);
        (void)app_ui_settings_show_wifi_connect_result(-1);
    }
}

static void app_business_wifi_select_task(void *arg)
{
    (void)arg;

    int ret = app_network_select_saved_wifi();
    (void)app_ui_settings_show_wifi_connect_result(ret);
    s_wifi_connect_busy = 0;
    s_network_switch_busy = 0;
    osal_task_delete_current();
}

static void app_business_4g_select_task(void *arg)
{
    (void)arg;

    int ret = -1;
    while (s_fake_4g_connect_busy) {
        ret = app_network_select_4g();
        if (ret == 0) {
            break;
        }
        osal_delay_ms(1000u);
    }

    (void)app_ui_settings_show_4g_select_result(ret == 0 ? UI_SETTINGS_4G_OK : UI_SETTINGS_4G_UNAVAILABLE, ret);
    s_fake_4g_connect_busy = 0;
    s_network_switch_busy = 0;
    osal_task_delete_current();
}

static void app_business_on_wifi_select(void)
{
    if (app_network_get_mode() == APP_NETWORK_MODE_WIFI) {
        return;
    }
    s_fake_4g_connect_busy = 0;
    if (s_wifi_connect_busy || s_network_switch_busy) {
        return;
    }

    s_wifi_connect_busy = 1;
    s_network_switch_busy = 1;
    if (osal_task_create("wifi_sel",
                         app_business_wifi_select_task,
                         NULL,
                         6144u,
                         4u,
                         NULL) != 0) {
        s_wifi_connect_busy = 0;
        s_network_switch_busy = 0;
        (void)app_ui_settings_show_wifi_connect_result(-1);
    }
}

static int app_business_on_4g_select(void)
{
    if (app_network_get_mode() == APP_NETWORK_MODE_4G || s_network_switch_busy || s_fake_4g_connect_busy) {
        return -1;
    }

    s_fake_4g_connect_busy = 1;
    s_network_switch_busy = 1;
    if (osal_task_create("fake_4g",
                         app_business_4g_select_task,
                         NULL,
                         6144u,
                         4u,
                         NULL) != 0) {
        s_fake_4g_connect_busy = 0;
        s_network_switch_busy = 0;
        (void)app_ui_settings_show_4g_select_result(UI_SETTINGS_4G_UNAVAILABLE, -1);
        return -2;
    }

    return 0;
}

static void app_business_on_power_key_long_press(void *user_data)
{
    (void)user_data;
    (void)app_ui_show_power_dialog();
}

static void app_business_on_power_shutdown_confirmed(void)
{
    (void)app_ui_prepare_shutdown_blackout();
    osal_delay_ms(120u);
    (void)d_power_control_shutdown();
}

/**
 * @brief 将 app_business 的回调函数注册到 UI 事件系统。
 *
 * UI 层只负责触发事件，不感知具体业务逻辑。
 * 回调函数指针在 ui_event.c 中以全局结构体 g_callbacks 存储。
 */
static void app_business_register_ui_callbacks(void)
{
    /* UI 层只发事件，具体业务由拆分后的 app 模块处理。 */
    ui_event_callbacks_t callbacks = {
        .intercom_channel_changed = app_business_on_channel_changed,
        .intercom_ptt_started = app_business_on_ptt_started,
        .intercom_ptt_stopped = app_business_on_ptt_stopped,
        .camera_entered = app_business_on_camera_entered,
        .camera_exited = app_business_on_camera_exited,
        .camera_capture_requested = app_business_on_camera_capture,
        .camera_upload_requested = app_business_on_camera_upload,
        .camera_retake_requested = app_business_on_camera_retake,
        .ai_question_started = app_business_on_ai_started,
        .ai_question_stopped = app_business_on_ai_stopped,
        .ai_reply_play_requested = app_business_on_ai_reply_play_requested,
        .ai_reply_stop_requested = app_business_on_ai_reply_stop_requested,
        .ai_cancel_requested = app_business_on_ai_cancel_requested,
        .settings_volume_changed = app_business_on_volume_changed,
        .settings_network_mode_get = app_business_get_network_mode,
        .settings_wifi_select_requested = app_business_on_wifi_select,
        .settings_wifi_scan_requested = app_business_on_wifi_scan,
        .settings_wifi_connect_requested = app_business_on_wifi_connect,
        .settings_4g_select_requested = app_business_on_4g_select,
        .settings_wifi_ssid_get = app_business_get_wifi_ssid,
        .power_shutdown_confirmed = app_business_on_power_shutdown_confirmed,
    };

    ui_event_set_callbacks(&callbacks);
    d_power_control_set_long_press_callback(app_business_on_power_key_long_press, NULL);
}

/* ==========================================================================
 * 业务启动与编排
 * ========================================================================== */

/**
 * @brief 第一版业务总启动入口。
 *
 * ## 启动顺序（严格有序，后面的依赖前面的）
 * 1. 创建音频会话互斥锁
 * 2. 注册 UI→业务回调（必须在 app 模块启动前完成，否则开机后用户操作可能丢失）
 * 3. 启动状态监控（电池 1s + 信号 3s 轮询任务）
 * 4. 启动对讲模块（UDP 连接 + PTT/RX/心跳三个任务）
 * 5. 启动 AI 语音模块（biz_ai 任务 + WAV 缓冲区分配）
 * 6. 启动相机业务模块
 *
 * ## 防重复
 * s_started 标志防止重复初始化，每个子模块内部也有各自的防重复检查。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_business_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = app_business_audio_session_init();
    if (ret != 0) {
        APP_LOGE(TAG, "音频会话状态初始化失败, ret=%d", ret);
        return ret;
    }

    /* 先注册 UI 回调，再启动后台业务，确保开机后用户操作能被接收。 */
    app_business_register_ui_callbacks();
    app_business_set_volume(s_volume, 0);

    ret = app_network_start();
    if (ret != 0) {
        APP_LOGE(TAG, "开机 WLAN 网络启动失败, ret=%d", ret);
        return ret;
    }

    ret = app_status_monitor_start();
    if (ret != 0) {
        return ret;
    }

    ret = app_business_start_volume_task();
    if (ret != 0) {
        APP_LOGE(TAG, "音量任务启动失败, ret=%d", ret);
        return ret;
    }

    service_buttons_callbacks_t button_callbacks = {
        .volume_step = app_business_on_volume_button_step,
    };
    ret = service_buttons_start(&button_callbacks);
    if (ret != 0) {
        APP_LOGE(TAG, "实体按键服务启动失败, ret=%d", ret);
        return ret;
    }

    ret = app_intercom_start();
    if (ret != 0) {
        return ret;
    }

    ret = app_ai_voice_start();
    if (ret != 0) {
        return ret;
    }

    ret = app_camera_start();
    if (ret != 0) {
        return ret;
    }

    s_started = 1;
    APP_LOGI(TAG, "业务启动完成, device=%s, server=%s:%d, channel=%d",
             APP_BUSINESS_DEVICE_NAME,
             APP_BUSINESS_SERVER_HOST,
             APP_BUSINESS_UDP_PORT,
             APP_BUSINESS_DEFAULT_CHANNEL);
    return 0;
}
