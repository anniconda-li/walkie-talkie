/**
 * @file app_business.c
 * @brief UDP 对讲、AI WAV 问答和 UI 状态监听业务控制器。
 */
#include "app_business.h"

#include "app_common.h"
#include "app_ui.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_battery.h"
#include "service_network.h"
#include "ui_event.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_business";

#define APP_BUSINESS_DEVICE_NAME        "walkie-01"
#define APP_BUSINESS_SERVER_HOST        "192.168.1.100"
#define APP_BUSINESS_UDP_PORT           9000
#define APP_BUSINESS_AI_HTTP_URL        "http://192.168.1.100:8080/ai/wav"
#define APP_BUSINESS_DEFAULT_CHANNEL    1

#define APP_BUSINESS_AUDIO_SAMPLE_RATE  16000u
#define APP_BUSINESS_AUDIO_BITS         16u
#define APP_BUSINESS_AUDIO_CHANNELS     1u
#define APP_BUSINESS_FRAME_SAMPLES      320u
#define APP_BUSINESS_FRAME_BYTES        (APP_BUSINESS_FRAME_SAMPLES * sizeof(int16_t))
#define APP_BUSINESS_AI_MAX_MS          2000u
#define APP_BUSINESS_AI_MAX_SAMPLES     ((APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AI_MAX_MS) / 1000u)
#define APP_BUSINESS_WAV_HEADER_LEN     44u
#define APP_BUSINESS_AI_WAV_MAX_BYTES   (APP_BUSINESS_WAV_HEADER_LEN + (APP_BUSINESS_AI_MAX_SAMPLES * sizeof(int16_t)))

#define APP_BUSINESS_PKT_MAGIC          "WTK1"
#define APP_BUSINESS_PKT_HEADER_LEN     34u
#define APP_BUSINESS_DEVICE_FIELD_LEN   16u
#define APP_BUSINESS_PKT_MAX_BYTES      (APP_BUSINESS_PKT_HEADER_LEN + APP_BUSINESS_FRAME_BYTES)

typedef enum {
    APP_BUSINESS_PKT_REGISTER = 1,
    APP_BUSINESS_PKT_CHANNEL = 2,
    APP_BUSINESS_PKT_PTT_START = 3,
    APP_BUSINESS_PKT_AUDIO = 4,
    APP_BUSINESS_PKT_PTT_STOP = 5,
    APP_BUSINESS_PKT_HEARTBEAT = 6,
} app_business_packet_type_t;

static osal_task_t s_ptt_task = NULL;
static osal_task_t s_ai_task = NULL;
static volatile int s_started = 0;
static volatile int s_network_ready = 0;
static volatile int s_udp_ready = 0;
static volatile int s_ptt_active = 0;
static volatile int s_ai_recording = 0;
static volatile int s_busy_audio_session = 0;
static int32_t s_current_channel = APP_BUSINESS_DEFAULT_CHANNEL;
static uint32_t s_udp_seq = 0u;
static union {
    uint8_t bytes[APP_BUSINESS_AI_WAV_MAX_BYTES];
    int16_t align;
} s_ai_wav_storage;

#define s_ai_wav_buf    (s_ai_wav_storage.bytes)

static void app_business_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t app_business_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static void app_business_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t app_business_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static int app_business_find_bytes(const uint8_t *buf, uint16_t len, const char *needle)
{
    uint16_t needle_len = (uint16_t)strlen(needle);
    if (buf == NULL || needle_len == 0u || len < needle_len) {
        return -1;
    }

    for (uint16_t i = 0; i <= (uint16_t)(len - needle_len); i++) {
        if (memcmp(&buf[i], needle, needle_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int app_business_csq_to_bars(const service_network_status_t *status)
{
    if (status == NULL || status->at_ready != 1 || status->sim_ready != 1 ||
        status->link_state != 1 || status->rssi < 0 || status->rssi == 99) {
        return 0;
    }

    if (status->rssi <= 9) {
        return 1;
    }
    if (status->rssi <= 14) {
        return 2;
    }
    if (status->rssi <= 19) {
        return 3;
    }
    return 4;
}

static uint32_t app_business_now_ms(void)
{
    return osal_get_tick_ms();
}

static uint16_t app_business_build_packet(uint8_t *out,
                                          uint8_t type,
                                          const uint8_t *payload,
                                          uint16_t payload_len)
{
    if (out == NULL || payload_len > APP_BUSINESS_FRAME_BYTES) {
        return 0u;
    }

    memcpy(&out[0], APP_BUSINESS_PKT_MAGIC, 4u);
    out[4] = type;
    out[5] = APP_BUSINESS_PKT_HEADER_LEN;
    app_business_write_u16(&out[6], (uint16_t)s_current_channel);
    app_business_write_u32(&out[8], s_udp_seq++);
    app_business_write_u32(&out[12], app_business_now_ms());
    memset(&out[16], 0, APP_BUSINESS_DEVICE_FIELD_LEN);
    strncpy((char *)&out[16], APP_BUSINESS_DEVICE_NAME, APP_BUSINESS_DEVICE_FIELD_LEN - 1u);
    app_business_write_u16(&out[32], payload_len);

    if (payload != NULL && payload_len > 0u) {
        memcpy(&out[APP_BUSINESS_PKT_HEADER_LEN], payload, payload_len);
    }

    return (uint16_t)(APP_BUSINESS_PKT_HEADER_LEN + payload_len);
}

static int app_business_send_control(uint8_t type)
{
    if (!s_network_ready || !s_udp_ready) {
        return -1;
    }

    uint8_t packet[APP_BUSINESS_PKT_HEADER_LEN];
    uint16_t len = app_business_build_packet(packet, type, NULL, 0u);
    return service_network_udp_send(packet, len);
}

static void app_business_write_wav_header(uint8_t *buf, uint32_t pcm_bytes)
{
    uint32_t byte_rate = APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint16_t block_align = APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint32_t riff_size = 36u + pcm_bytes;

    memcpy(&buf[0], "RIFF", 4u);
    app_business_write_u32(&buf[4], riff_size);
    memcpy(&buf[8], "WAVEfmt ", 8u);
    app_business_write_u32(&buf[16], 16u);
    app_business_write_u16(&buf[20], 1u);
    app_business_write_u16(&buf[22], APP_BUSINESS_AUDIO_CHANNELS);
    app_business_write_u32(&buf[24], APP_BUSINESS_AUDIO_SAMPLE_RATE);
    app_business_write_u32(&buf[28], byte_rate);
    app_business_write_u16(&buf[32], block_align);
    app_business_write_u16(&buf[34], APP_BUSINESS_AUDIO_BITS);
    memcpy(&buf[36], "data", 4u);
    app_business_write_u32(&buf[40], pcm_bytes);
}

static int app_business_parse_wav(const uint8_t *wav,
                                  uint16_t wav_len,
                                  const int16_t **pcm,
                                  uint32_t *samples)
{
    if (wav == NULL || wav_len < APP_BUSINESS_WAV_HEADER_LEN || pcm == NULL || samples == NULL) {
        return -1;
    }
    if (memcmp(&wav[0], "RIFF", 4u) != 0 || memcmp(&wav[8], "WAVE", 4u) != 0) {
        return -2;
    }

    uint16_t audio_format = app_business_read_u16(&wav[20]);
    uint16_t channels = app_business_read_u16(&wav[22]);
    uint32_t sample_rate = app_business_read_u32(&wav[24]);
    uint16_t bits = app_business_read_u16(&wav[34]);

    if (audio_format != 1u || channels != APP_BUSINESS_AUDIO_CHANNELS ||
        sample_rate != APP_BUSINESS_AUDIO_SAMPLE_RATE || bits != APP_BUSINESS_AUDIO_BITS) {
        return -3;
    }

    uint32_t data_offset = 36u;
    while ((data_offset + 8u) <= wav_len) {
        uint32_t chunk_size = app_business_read_u32(&wav[data_offset + 4u]);
        if (memcmp(&wav[data_offset], "data", 4u) == 0) {
            if ((data_offset + 8u + chunk_size) > wav_len) {
                return -4;
            }
            *pcm = (const int16_t *)&wav[data_offset + 8u];
            *samples = chunk_size / sizeof(int16_t);
            return 0;
        }
        data_offset += 8u + chunk_size + (chunk_size & 1u);
    }

    return -5;
}

static void app_business_battery_task(void *arg)
{
    (void)arg;

    while (1) {
        int voltage_mv = 0;
        int percent = 0;
        if (service_battery_get_status(&voltage_mv, &percent) == 0) {
            (void)app_ui_set_battery_level(percent);
        }
        osal_delay_ms(1000u);
    }
}

static void app_business_network_task(void *arg)
{
    (void)arg;

    while (1) {
        service_network_status_t status;
        if (service_network_get_status(&status) == 0) {
            int bars = app_business_csq_to_bars(&status);
            (void)app_ui_set_network_state(bars);
            s_network_ready = bars > 0 ? 1 : 0;
        } else {
            (void)app_ui_set_network_state(0);
            s_network_ready = 0;
            (void)service_network_init(NULL);
        }

        osal_delay_ms(3000u);
    }
}

static void app_business_heartbeat_task(void *arg)
{
    (void)arg;
    (void)app_business_send_control(APP_BUSINESS_PKT_REGISTER);
    (void)app_business_send_control(APP_BUSINESS_PKT_CHANNEL);

    while (1) {
        if (s_network_ready && !s_udp_ready) {
            int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
            if (ret == 0) {
                s_udp_ready = 1;
                (void)app_business_send_control(APP_BUSINESS_PKT_REGISTER);
                (void)app_business_send_control(APP_BUSINESS_PKT_CHANNEL);
            }
        }
        (void)app_business_send_control(APP_BUSINESS_PKT_HEARTBEAT);
        osal_delay_ms(10000u);
    }
}

static int app_business_is_own_packet(const uint8_t *packet)
{
    char name[APP_BUSINESS_DEVICE_FIELD_LEN + 1u];
    memcpy(name, &packet[16], APP_BUSINESS_DEVICE_FIELD_LEN);
    name[APP_BUSINESS_DEVICE_FIELD_LEN] = '\0';
    return strncmp(name, APP_BUSINESS_DEVICE_NAME, APP_BUSINESS_DEVICE_FIELD_LEN) == 0;
}

static void app_business_handle_udp_packet(const uint8_t *packet, uint16_t len)
{
    if (len < APP_BUSINESS_PKT_HEADER_LEN || memcmp(packet, APP_BUSINESS_PKT_MAGIC, 4u) != 0) {
        return;
    }

    uint8_t type = packet[4];
    uint8_t header_len = packet[5];
    uint16_t channel = app_business_read_u16(&packet[6]);
    uint32_t seq = app_business_read_u32(&packet[8]);
    uint16_t payload_len = app_business_read_u16(&packet[32]);

    if (header_len != APP_BUSINESS_PKT_HEADER_LEN ||
        channel != (uint16_t)s_current_channel ||
        len < (uint16_t)(header_len + payload_len) ||
        app_business_is_own_packet(packet)) {
        return;
    }

    if (type == APP_BUSINESS_PKT_AUDIO && payload_len > 0u && s_busy_audio_session == 0) {
        int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];
        uint16_t copy_len = payload_len > sizeof(pcm) ? sizeof(pcm) : payload_len;
        memcpy(pcm, &packet[header_len], copy_len);
        int played = service_audio_play(pcm, copy_len / sizeof(int16_t), 30u);
        if (played < 0) {
            APP_LOGW(TAG, "UDP 音频播放失败, seq=%u, ret=%d", (unsigned int)seq, played);
        }
    }
}

static void app_business_udp_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[APP_BUSINESS_PKT_MAX_BYTES * 2u];
    uint16_t used = 0u;

    while (1) {
        int ret = service_network_read_downlink(&rx[used], (uint16_t)(sizeof(rx) - used), 80u);
        if (ret > 0) {
            used = (uint16_t)(used + ret);
            uint16_t pos = 0u;
            while ((pos + APP_BUSINESS_PKT_HEADER_LEN) <= used) {
                if (memcmp(&rx[pos], APP_BUSINESS_PKT_MAGIC, 4u) != 0) {
                    pos++;
                    continue;
                }

                uint8_t header_len = rx[pos + 5u];
                uint16_t payload_len = app_business_read_u16(&rx[pos + 32u]);
                uint16_t packet_len = (uint16_t)(header_len + payload_len);
                if (header_len != APP_BUSINESS_PKT_HEADER_LEN || packet_len > APP_BUSINESS_PKT_MAX_BYTES) {
                    pos++;
                    continue;
                }
                if ((pos + packet_len) > used) {
                    break;
                }

                app_business_handle_udp_packet(&rx[pos], packet_len);
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

static void app_business_ptt_task(void *arg)
{
    (void)arg;
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];
    uint8_t packet[APP_BUSINESS_PKT_MAX_BYTES];

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ptt_active || s_ai_recording || s_busy_audio_session) {
            continue;
        }

        s_busy_audio_session = 1;
        (void)app_business_send_control(APP_BUSINESS_PKT_PTT_START);
        while (s_ptt_active) {
            int samples = service_audio_read(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
            if (samples > 0) {
                uint16_t payload_len = (uint16_t)(samples * sizeof(int16_t));
                uint16_t packet_len = app_business_build_packet(packet,
                                                                APP_BUSINESS_PKT_AUDIO,
                                                                (const uint8_t *)pcm,
                                                                payload_len);
                (void)service_network_udp_send(packet, packet_len);
            }
        }
        (void)app_business_send_control(APP_BUSINESS_PKT_PTT_STOP);
        s_busy_audio_session = 0;
    }
}

static void app_business_ai_task(void *arg)
{
    (void)arg;

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ai_recording || s_ptt_active || s_busy_audio_session) {
            s_ai_recording = 0;
            continue;
        }

        s_busy_audio_session = 1;
        uint32_t samples_total = 0u;
        int16_t *pcm = (int16_t *)&s_ai_wav_buf[APP_BUSINESS_WAV_HEADER_LEN];
        while (s_ai_recording && samples_total < APP_BUSINESS_AI_MAX_SAMPLES) {
            uint32_t remain = APP_BUSINESS_AI_MAX_SAMPLES - samples_total;
            uint32_t request = remain > APP_BUSINESS_FRAME_SAMPLES ? APP_BUSINESS_FRAME_SAMPLES : remain;
            int samples = service_audio_read(&pcm[samples_total], request, 30u);
            if (samples > 0) {
                samples_total += (uint32_t)samples;
            }
        }
        s_ai_recording = 0;

        uint32_t pcm_bytes = samples_total * sizeof(int16_t);
        if (pcm_bytes > 0u) {
            app_business_write_wav_header(s_ai_wav_buf, pcm_bytes);
            uint16_t wav_len = (uint16_t)(APP_BUSINESS_WAV_HEADER_LEN + pcm_bytes);
            uint16_t resp_len = 0u;
            int ret = service_network_http_post_wav(APP_BUSINESS_AI_HTTP_URL,
                                                    s_ai_wav_buf,
                                                    wav_len,
                                                    s_ai_wav_buf,
                                                    sizeof(s_ai_wav_buf),
                                                    &resp_len);
            if (ret == 0 && resp_len > 0u) {
                const int16_t *resp_pcm = NULL;
                uint32_t resp_samples = 0u;
                int riff_pos = app_business_find_bytes(s_ai_wav_buf, resp_len, "RIFF");
                if (riff_pos > 0) {
                    memmove(s_ai_wav_buf, &s_ai_wav_buf[riff_pos], resp_len - (uint16_t)riff_pos);
                    resp_len = (uint16_t)(resp_len - (uint16_t)riff_pos);
                }
                ret = app_business_parse_wav(s_ai_wav_buf, resp_len, &resp_pcm, &resp_samples);
                if (ret == 0 && resp_samples > 0u) {
                    (void)service_audio_play(resp_pcm, resp_samples, 100u);
                } else {
                    APP_LOGW(TAG, "AI 响应 WAV 解析失败, ret=%d, len=%u", ret, (unsigned int)resp_len);
                }
            }
        }

        s_busy_audio_session = 0;
    }
}

static void app_business_on_channel_changed(int32_t channel)
{
    if (channel <= 0) {
        channel = APP_BUSINESS_DEFAULT_CHANNEL;
    }
    s_current_channel = channel;
    (void)app_business_send_control(APP_BUSINESS_PKT_CHANNEL);
}

static void app_business_on_ptt_started(int32_t channel)
{
    s_current_channel = channel > 0 ? channel : s_current_channel;
    if (s_ai_recording || s_busy_audio_session) {
        return;
    }

    s_ptt_active = 1;
    if (s_ptt_task != NULL) {
        (void)osal_task_notify_give(s_ptt_task);
    }
}

static void app_business_on_ptt_stopped(int32_t channel)
{
    (void)channel;
    s_ptt_active = 0;
}

static void app_business_on_ai_started(void)
{
    if (s_ptt_active || s_busy_audio_session) {
        return;
    }

    s_ai_recording = 1;
    if (s_ai_task != NULL) {
        (void)osal_task_notify_give(s_ai_task);
    }
}

static void app_business_on_ai_stopped(void)
{
    s_ai_recording = 0;
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

    int ret = service_battery_init();
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
    } else {
        ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
        if (ret != 0) {
            APP_LOGW(TAG, "UDP 对讲通道初始化失败, ret=%d", ret);
        } else {
            s_network_ready = 1;
            s_udp_ready = 1;
        }
    }

    app_business_register_ui_callbacks();

    (void)osal_task_create("biz_battery", app_business_battery_task, NULL, 3072u, 5u, NULL);
    (void)osal_task_create("biz_network", app_business_network_task, NULL, 4096u, 4u, NULL);
    (void)osal_task_create("biz_heartbeat", app_business_heartbeat_task, NULL, 3072u, 4u, NULL);
    (void)osal_task_create("biz_udp_rx", app_business_udp_rx_task, NULL, 4096u, 5u, NULL);
    (void)osal_task_create("biz_ptt", app_business_ptt_task, NULL, 4096u, 6u, &s_ptt_task);
    (void)osal_task_create("biz_ai", app_business_ai_task, NULL, 6144u, 5u, &s_ai_task);

    s_started = 1;
    APP_LOGI(TAG, "业务控制器启动完成, device=%s, server=%s:%d, channel=%ld",
             APP_BUSINESS_DEVICE_NAME,
             APP_BUSINESS_SERVER_HOST,
             APP_BUSINESS_UDP_PORT,
             (long)s_current_channel);
    return 0;
}
