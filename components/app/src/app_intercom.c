/**
 * @file app_intercom.c
 * @brief UDP 实时对讲业务。
 */
#include "app_intercom.h"

#include "app_business.h"
#include "app_business_config.h"
#include "app_common.h"
#include "app_status_monitor.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "app_intercom";

#define APP_INTERCOM_PACKET_MAGIC       "WTK1"
#define APP_INTERCOM_DEVICE_FIELD_LEN   16u
#define APP_INTERCOM_PACKET_HEADER_LEN  34u
#define APP_INTERCOM_PACKET_MAX_PAYLOAD APP_BUSINESS_FRAME_BYTES
#define APP_INTERCOM_PACKET_MAX_BYTES   (APP_INTERCOM_PACKET_HEADER_LEN + APP_INTERCOM_PACKET_MAX_PAYLOAD)
#define APP_INTERCOM_PTT_TASK_STACK     6144u
#define APP_INTERCOM_RX_TASK_STACK      4096u
#define APP_INTERCOM_HEARTBEAT_STACK    3072u

typedef enum {
    APP_INTERCOM_PKT_REGISTER = 1,
    APP_INTERCOM_PKT_CHANNEL = 2,
    APP_INTERCOM_PKT_PTT_START = 3,
    APP_INTERCOM_PKT_AUDIO = 4,
    APP_INTERCOM_PKT_PTT_STOP = 5,
    APP_INTERCOM_PKT_HEARTBEAT = 6,
} app_intercom_packet_type_t;

typedef struct {
    uint8_t type;
    uint16_t channel;
    uint32_t seq;
    const uint8_t *payload;
    uint16_t payload_len;
    const uint8_t *packet;
    uint16_t packet_len;
} app_intercom_packet_view_t;

static osal_task_t s_ptt_task = NULL;
static volatile int s_started = 0;
static volatile int s_udp_ready = 0;
static volatile int s_ptt_active = 0;
static int32_t s_current_channel = APP_BUSINESS_DEFAULT_CHANNEL;
static uint32_t s_udp_seq = 0u;

/** @brief 写入对讲协议 little-endian uint16 字段。 */
static void app_intercom_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

/** @brief 读取对讲协议 little-endian uint16 字段。 */
static uint16_t app_intercom_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/** @brief 写入对讲协议 little-endian uint32 字段。 */
static void app_intercom_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

/** @brief 读取对讲协议 little-endian uint32 字段。 */
static uint32_t app_intercom_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static uint16_t app_intercom_build_packet(uint8_t *out,
                                          uint8_t type,
                                          const uint8_t *payload,
                                          uint16_t payload_len)
{
    if (out == NULL || payload_len > APP_INTERCOM_PACKET_MAX_PAYLOAD) {
        return 0u;
    }

    memcpy(&out[0], APP_INTERCOM_PACKET_MAGIC, 4u);
    out[4] = type;
    out[5] = APP_INTERCOM_PACKET_HEADER_LEN;
    app_intercom_write_u16(&out[6], (uint16_t)s_current_channel);
    app_intercom_write_u32(&out[8], s_udp_seq++);
    app_intercom_write_u32(&out[12], osal_get_tick_ms());
    /* 设备名固定 16 字节，短名后面补 0，便于服务器原样转发和客户端比较。 */
    memset(&out[16], 0, APP_INTERCOM_DEVICE_FIELD_LEN);
    strncpy((char *)&out[16], APP_BUSINESS_DEVICE_NAME, APP_INTERCOM_DEVICE_FIELD_LEN - 1u);
    app_intercom_write_u16(&out[32], payload_len);

    if (payload != NULL && payload_len > 0u) {
        memcpy(&out[APP_INTERCOM_PACKET_HEADER_LEN], payload, payload_len);
    }

    return (uint16_t)(APP_INTERCOM_PACKET_HEADER_LEN + payload_len);
}

static int app_intercom_parse_packet(const uint8_t *packet,
                                     uint16_t len,
                                     app_intercom_packet_view_t *view)
{
    if (packet == NULL || view == NULL || len < APP_INTERCOM_PACKET_HEADER_LEN) {
        return -1;
    }
    if (memcmp(packet, APP_INTERCOM_PACKET_MAGIC, 4u) != 0) {
        return -2;
    }

    uint8_t header_len = packet[5];
    uint16_t payload_len = app_intercom_read_u16(&packet[32]);
    /* 解析阶段只建立 view，不复制 payload，减少 20ms 音频包处理开销。 */
    if (header_len != APP_INTERCOM_PACKET_HEADER_LEN ||
        len < (uint16_t)(header_len + payload_len)) {
        return -3;
    }

    view->type = packet[4];
    view->channel = app_intercom_read_u16(&packet[6]);
    view->seq = app_intercom_read_u32(&packet[8]);
    view->payload = &packet[header_len];
    view->payload_len = payload_len;
    view->packet = packet;
    view->packet_len = (uint16_t)(header_len + payload_len);
    return 0;
}

static int app_intercom_packet_is_own(const uint8_t *packet)
{
    char name[APP_INTERCOM_DEVICE_FIELD_LEN + 1u];

    if (packet == NULL) {
        return 0;
    }

    memcpy(name, &packet[16], APP_INTERCOM_DEVICE_FIELD_LEN);
    name[APP_INTERCOM_DEVICE_FIELD_LEN] = '\0';
    return strncmp(name, APP_BUSINESS_DEVICE_NAME, APP_INTERCOM_DEVICE_FIELD_LEN) == 0;
}

static int app_intercom_send_control(uint8_t type)
{
    if (!s_udp_ready) {
        return -1;
    }

    /* 控制包没有 payload，用于服务器维护设备在线状态和频道状态。 */
    uint8_t packet[APP_INTERCOM_PACKET_HEADER_LEN];
    uint16_t len = app_intercom_build_packet(packet, type, NULL, 0u);
    return service_network_udp_send(packet, len);
}

static void app_intercom_heartbeat_task(void *arg)
{
    (void)arg;
    (void)app_intercom_send_control(APP_INTERCOM_PKT_REGISTER);
    (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);

    while (1) {
        /* 网络恢复后在后台重建 UDP DTU 通道，并重新上报设备和频道。 */
        if (app_status_monitor_network_ready() && !s_udp_ready) {
            int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
            if (ret == 0) {
                s_udp_ready = 1;
                (void)app_intercom_send_control(APP_INTERCOM_PKT_REGISTER);
                (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);
            }
        }
        (void)app_intercom_send_control(APP_INTERCOM_PKT_HEARTBEAT);
        osal_delay_ms(10000u);
    }
}

static void app_intercom_handle_udp_packet(const uint8_t *packet, uint16_t len)
{
    app_intercom_packet_view_t view;
    if (app_intercom_parse_packet(packet, len, &view) != 0) {
        return;
    }
    /* 服务器会原样转发音频包，本机自己的包和非当前频道包都直接忽略。 */
    if (view.channel != (uint16_t)s_current_channel ||
        app_intercom_packet_is_own(packet)) {
        return;
    }

    if (view.type == APP_INTERCOM_PKT_AUDIO &&
        view.payload_len > 0u) {
        int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];
        uint16_t copy_len = view.payload_len > sizeof(pcm) ? sizeof(pcm) : view.payload_len;
        memcpy(pcm, view.payload, copy_len);
        (void)service_audio_start_playback();
        int played = service_audio_play(pcm, copy_len / sizeof(int16_t), 30u);
        if (played < 0) {
            APP_LOGW(TAG, "UDP 音频播放失败, seq=%u, ret=%d", (unsigned int)view.seq, played);
        }
    }
}

static void app_intercom_udp_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[APP_INTERCOM_PACKET_MAX_BYTES * 2u];
    uint16_t used = 0u;

    while (1) {
        int ret = service_network_read_downlink(&rx[used], (uint16_t)(sizeof(rx) - used), 80u);
        if (ret > 0) {
            used = (uint16_t)(used + ret);
            uint16_t pos = 0u;
            /* UART 透传数据可能跨多次读取，保留半包并只消费完整 WTK1 包。 */
            while ((pos + APP_INTERCOM_PACKET_HEADER_LEN) <= used) {
                if (memcmp(&rx[pos], APP_INTERCOM_PACKET_MAGIC, 4u) != 0) {
                    pos++;
                    continue;
                }

                uint8_t header_len = rx[pos + 5u];
                uint16_t payload_len = (uint16_t)rx[pos + 32u] | ((uint16_t)rx[pos + 33u] << 8);
                uint16_t packet_len = (uint16_t)(header_len + payload_len);
                if (header_len != APP_INTERCOM_PACKET_HEADER_LEN || packet_len > APP_INTERCOM_PACKET_MAX_BYTES) {
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
    uint8_t packet[APP_INTERCOM_PACKET_MAX_BYTES];

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ptt_active || app_business_audio_session_try_begin() != 0) {
            continue;
        }

        /* PTT 期间按固定 20ms PCM 帧发送，第一版不做编解码和重传。 */
        (void)app_intercom_send_control(APP_INTERCOM_PKT_PTT_START);
        uint32_t read_ok = 0u;
        uint32_t read_fail = 0u;
        uint32_t send_ok = 0u;
        uint32_t send_fail = 0u;
        int last_read_ret = 0;
        int last_send_ret = 0;
        while (s_ptt_active) {
            int samples = service_audio_read(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
            if (samples > 0) {
                read_ok++;
                uint16_t payload_len = (uint16_t)(samples * sizeof(int16_t));
                uint16_t packet_len = app_intercom_build_packet(packet,
                                                                APP_INTERCOM_PKT_AUDIO,
                                                                (const uint8_t *)pcm,
                                                                payload_len);
                if (packet_len > 0u) {
                    int send_ret = service_network_udp_send(packet, packet_len);
                    if (send_ret == 0) {
                        send_ok++;
                    } else {
                        send_fail++;
                        last_send_ret = send_ret;
                    }
                } else {
                    send_fail++;
                    last_send_ret = -100;
                }
            } else {
                read_fail++;
                last_read_ret = samples;
            }
        }
        (void)app_intercom_send_control(APP_INTERCOM_PKT_PTT_STOP);
        APP_LOGI(TAG,
                 "PTT 发送统计: read_ok=%u, read_fail=%u, send_ok=%u, send_fail=%u, "
                 "last_read=%d, last_send=%d",
                 (unsigned int)read_ok,
                 (unsigned int)read_fail,
                 (unsigned int)send_ok,
                 (unsigned int)send_fail,
                 last_read_ret,
                 last_send_ret);
        app_business_audio_session_end();
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

    ret = osal_task_create("biz_ptt",
                           app_intercom_ptt_task,
                           NULL,
                           APP_INTERCOM_PTT_TASK_STACK,
                           6u,
                           &s_ptt_task);
    if (ret != 0) {
        APP_LOGE(TAG, "PTT 任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_udp_rx",
                           app_intercom_udp_rx_task,
                           NULL,
                           APP_INTERCOM_RX_TASK_STACK,
                           5u,
                           NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "UDP 接收任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_heartbeat",
                           app_intercom_heartbeat_task,
                           NULL,
                           APP_INTERCOM_HEARTBEAT_STACK,
                           4u,
                           NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "对讲心跳任务启动失败, ret=%d", ret);
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
    (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);
}

void app_intercom_ptt_start(int32_t channel)
{
    s_current_channel = channel > 0 ? channel : s_current_channel;
    if (app_business_audio_session_is_busy()) {
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
