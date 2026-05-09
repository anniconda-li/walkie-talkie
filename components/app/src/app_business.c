/**
 * @file app_business.c
 * @brief 第一版业务启动编排和 UI 事件转发。
 */
#include "app_business.h"

#include "app_ai_voice.h"
#include "app_business_config.h"
#include "app_common.h"
#include "app_intercom.h"
#include "app_status_monitor.h"
#include "app_ui.h"
#include "osal_mutex.h"
#include "service_audio.h"
#include "service_screen.h"
#include "ui_event.h"

#include <stdint.h>
#include <stddef.h>

static const char *TAG = "app_business";

static volatile int s_started = 0;
static osal_mutex_t s_audio_session_mutex = NULL;
static int s_audio_session_busy = 0;

static int app_business_audio_session_init(void)
{
    if (s_audio_session_mutex != NULL) {
        return 0;
    }

    s_audio_session_mutex = osal_mutex_create();
    return s_audio_session_mutex == NULL ? -1 : 0;
}

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

static void app_business_on_channel_changed(int32_t channel)
{
    app_intercom_set_channel(channel);
}

static void app_business_on_ptt_started(int32_t channel)
{
    app_intercom_ptt_start(channel);
}

static void app_business_on_ptt_stopped(int32_t channel)
{
    (void)channel;
    app_intercom_ptt_stop();
}

static void app_business_on_ai_started(void)
{
    app_ai_voice_record_start();
}

static void app_business_on_ai_stopped(void)
{
    app_ai_voice_record_stop();
}

static void app_business_on_volume_changed(int32_t value)
{
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }

    (void)service_audio_set_volume((uint8_t)value);
}

static void app_business_register_ui_callbacks(void)
{
    /* UI 层只发事件，具体业务由拆分后的 app 模块处理。 */
    ui_event_callbacks_t callbacks = {
        .intercom_channel_changed = app_business_on_channel_changed,
        .intercom_ptt_started = app_business_on_ptt_started,
        .intercom_ptt_stopped = app_business_on_ptt_stopped,
        .ai_question_started = app_business_on_ai_started,
        .ai_question_stopped = app_business_on_ai_stopped,
        .settings_volume_changed = app_business_on_volume_changed,
    };

    ui_event_set_callbacks(&callbacks);
}

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

    ret = app_ui_create();
    if (ret != 0) {
        APP_LOGE(TAG, "应用 UI 创建失败, ret=%d", ret);
        return ret;
    }

    /* 先注册 UI 回调，再启动后台业务，确保开机后用户操作能被接收。 */
    app_business_register_ui_callbacks();

    ret = app_status_monitor_start();
    if (ret != 0) {
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

    s_started = 1;
    APP_LOGI(TAG, "业务启动完成, device=%s, server=%s:%d, channel=%d",
             APP_BUSINESS_DEVICE_NAME,
             APP_BUSINESS_SERVER_HOST,
             APP_BUSINESS_UDP_PORT,
             APP_BUSINESS_DEFAULT_CHANNEL);
    return 0;
}
