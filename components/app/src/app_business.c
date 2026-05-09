/**
 * @file app_business.c
 * @brief 第一版业务启动编排和 UI 事件转发。
 */
#include "app_business.h"

#include "app_ai_voice.h"
#include "app_audio_session.h"
#include "app_business_config.h"
#include "app_common.h"
#include "app_intercom.h"
#include "app_status_monitor.h"
#include "service_audio.h"
#include "service_battery.h"
#include "service_network.h"
#include "ui_event.h"

#include <stdint.h>

static const char *TAG = "app_business";

static volatile int s_started = 0;

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

    int ret = app_audio_session_init();
    if (ret != 0) {
        APP_LOGE(TAG, "音频会话状态初始化失败, ret=%d", ret);
        return ret;
    }

    ret = service_battery_init();
    if (ret != 0) {
        APP_LOGE(TAG, "电池服务初始化失败, ret=%d", ret);
        return ret;
    }

    service_audio_config_t audio_cfg = {
        .volume = 80u,
        .passthrough_gain = 1u,
        .input = SERVICE_AUDIO_INPUT_MIC1,
    };
    ret = service_audio_init(&audio_cfg);
    if (ret != 0) {
        APP_LOGE(TAG, "音频服务初始化失败, ret=%d", ret);
        return ret;
    }

    ret = service_network_init(NULL);
    if (ret != 0) {
        APP_LOGW(TAG, "网络服务初始化失败，后台状态任务仍会显示无信号, ret=%d", ret);
    }

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
