/**
 * @file app_intercom.c
 * @brief UDP 实时对讲业务。
 */
#include "app_intercom.h"

#include "app_audio_session.h"
#include "app_business_config.h"
#include "app_common.h"
#include "app_status_monitor.h"
#include "app_walkie_protocol.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "app_intercom";

static osal_task_t s_ptt_task = NULL;
static volatile int s_started = 0;
static volatile int s_udp_ready = 0;
static volatile int s_ptt_active = 0;
static int32_t s_current_channel = APP_BUSINESS_DEFAULT_CHANNEL;
static uint32_t s_udp_seq = 0u;

static uint16_t app_intercom_build_packet(uint8_t *out,
                                          uint8_t type,
                                          const uint8_t *payload,
                                          uint16_t payload_len)
{
    return app_walkie_build_packet(out,
                                   type,
                                   (uint16_t)s_current_channel,
                                   s_udp_seq++,
                                   APP_BUSINESS_DEVICE_NAME,
                                   payload,
                                   payload_len);
}

static int app_intercom_send_control(uint8_t type)
{
    if (!s_udp_ready) {
        return -1;
    }

    uint8_t packet[APP_WALKIE_PACKET_HEADER_LEN];
    uint16_t len = app_intercom_build_packet(packet, type, NULL, 0u);
    return service_network_udp_send(packet, len);
}

static void app_intercom_heartbeat_task(void *arg)
{
    (void)arg;
    (void)app_intercom_send_control(APP_WALKIE_PKT_REGISTER);
    (void)app_intercom_send_control(APP_WALKIE_PKT_CHANNEL);

    while (1) {
        if (app_status_monitor_network_ready() && !s_udp_ready) {
            int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
            if (ret == 0) {
                s_udp_ready = 1;
                (void)app_intercom_send_control(APP_WALKIE_PKT_REGISTER);
                (void)app_intercom_send_control(APP_WALKIE_PKT_CHANNEL);
            }
        }
        (void)app_intercom_send_control(APP_WALKIE_PKT_HEARTBEAT);
        osal_delay_ms(10000u);
    }
}

static void app_intercom_handle_udp_packet(const uint8_t *packet, uint16_t len)
{
    app_walkie_packet_view_t view;
    if (app_walkie_parse_packet(packet, len, &view) != 0) {
        return;
    }
    if (view.channel != (uint16_t)s_current_channel ||
        app_walkie_packet_is_from_device(packet, APP_BUSINESS_DEVICE_NAME)) {
        return;
    }

    if (view.type == APP_WALKIE_PKT_AUDIO &&
        view.payload_len > 0u &&
        !app_audio_session_is_busy()) {
        int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];
        uint16_t copy_len = view.payload_len > sizeof(pcm) ? sizeof(pcm) : view.payload_len;
        memcpy(pcm, view.payload, copy_len);
        int played = service_audio_play(pcm, copy_len / sizeof(int16_t), 30u);
        if (played < 0) {
            APP_LOGW(TAG, "UDP 音频播放失败, seq=%u, ret=%d", (unsigned int)view.seq, played);
        }
    }
}

static void app_intercom_udp_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[APP_WALKIE_PACKET_MAX_BYTES * 2u];
    uint16_t used = 0u;

    while (1) {
        int ret = service_network_read_downlink(&rx[used], (uint16_t)(sizeof(rx) - used), 80u);
        if (ret > 0) {
            used = (uint16_t)(used + ret);
            uint16_t pos = 0u;
            while ((pos + APP_WALKIE_PACKET_HEADER_LEN) <= used) {
                if (memcmp(&rx[pos], "WTK1", 4u) != 0) {
                    pos++;
                    continue;
                }

                uint8_t header_len = rx[pos + 5u];
                uint16_t payload_len = (uint16_t)rx[pos + 32u] | ((uint16_t)rx[pos + 33u] << 8);
                uint16_t packet_len = (uint16_t)(header_len + payload_len);
                if (header_len != APP_WALKIE_PACKET_HEADER_LEN || packet_len > APP_WALKIE_PACKET_MAX_BYTES) {
                    pos++;
                    continue;
                }
                if ((pos + packet_len) > used) {
                    break;
                }

                app_intercom_handle_udp_packet(&rx[pos], packet_len);
                pos = (uint16_t)(pos + packet_len);
            }

            if (pos > 0u) {
                memmove(rx, &rx[pos], used - pos);
                used = (uint16_t)(used - pos);
            }
            if (used >= sizeof(rx)) {
                used = 0u;
            }
        } else {
            osal_delay_ms(10u);
        }
    }
}

static void app_intercom_ptt_task(void *arg)
{
    (void)arg;
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];
    uint8_t packet[APP_WALKIE_PACKET_MAX_BYTES];

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ptt_active || app_audio_session_try_begin() != 0) {
            continue;
        }

        (void)app_intercom_send_control(APP_WALKIE_PKT_PTT_START);
        while (s_ptt_active) {
            int samples = service_audio_read(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
            if (samples > 0) {
                uint16_t payload_len = (uint16_t)(samples * sizeof(int16_t));
                uint16_t packet_len = app_intercom_build_packet(packet,
                                                                APP_WALKIE_PKT_AUDIO,
                                                                (const uint8_t *)pcm,
                                                                payload_len);
                (void)service_network_udp_send(packet, packet_len);
            }
        }
        (void)app_intercom_send_control(APP_WALKIE_PKT_PTT_STOP);
        app_audio_session_end();
    }
}

int app_intercom_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
    if (ret != 0) {
        APP_LOGW(TAG, "UDP 对讲通道初始化失败, ret=%d", ret);
    } else {
        s_udp_ready = 1;
    }

    ret = osal_task_create("biz_heartbeat", app_intercom_heartbeat_task, NULL, 3072u, 4u, NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "对讲心跳任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_udp_rx", app_intercom_udp_rx_task, NULL, 4096u, 5u, NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "UDP 接收任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_ptt", app_intercom_ptt_task, NULL, 4096u, 6u, &s_ptt_task);
    if (ret != 0) {
        APP_LOGE(TAG, "PTT 任务启动失败, ret=%d", ret);
        return ret;
    }

    s_started = 1;
    return 0;
}

void app_intercom_set_channel(int32_t channel)
{
    if (channel <= 0) {
        channel = APP_BUSINESS_DEFAULT_CHANNEL;
    }
    s_current_channel = channel;
    (void)app_intercom_send_control(APP_WALKIE_PKT_CHANNEL);
}

void app_intercom_ptt_start(int32_t channel)
{
    s_current_channel = channel > 0 ? channel : s_current_channel;
    if (app_audio_session_is_busy()) {
        return;
    }

    s_ptt_active = 1;
    if (s_ptt_task != NULL) {
        (void)osal_task_notify_give(s_ptt_task);
    }
}

void app_intercom_ptt_stop(void)
{
    s_ptt_active = 0;
}
