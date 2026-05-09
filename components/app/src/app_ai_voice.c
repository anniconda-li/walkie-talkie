/**
 * @file app_ai_voice.c
 * @brief AI 语音问答业务。
 */
#include "app_ai_voice.h"

#include "app_business.h"
#include "app_business_config.h"
#include "app_common.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "app_ai_voice";

static osal_task_t s_ai_task = NULL;
static volatile int s_started = 0;
static volatile int s_ai_recording = 0;

/**
 * @brief AI 请求和响应复用缓冲区。
 *
 * 请求阶段保存 WAV header + PCM；响应阶段保存服务器返回的 WAV body。
 * union 中的 int16_t 成员用于保证 PCM 指针至少具备 16-bit 对齐。
 */
static union {
    uint8_t bytes[APP_BUSINESS_AI_WAV_MAX_BYTES];
    int16_t align;
} s_ai_wav_storage;

#define s_ai_wav_buf    (s_ai_wav_storage.bytes)

/** @brief 写入 WAV little-endian uint16 字段。 */
static void app_ai_voice_wav_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

/** @brief 读取 WAV little-endian uint16 字段。 */
static uint16_t app_ai_voice_wav_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/** @brief 写入 WAV little-endian uint32 字段。 */
static void app_ai_voice_wav_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

/** @brief 读取 WAV little-endian uint32 字段。 */
static uint32_t app_ai_voice_wav_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static int app_ai_voice_find_riff(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len < 4u) {
        return -1;
    }

    for (uint16_t i = 0; i <= (uint16_t)(len - 4u); i++) {
        if (memcmp(&buf[i], "RIFF", 4u) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static void app_ai_voice_wav_write_header(uint8_t *buf, uint32_t pcm_bytes)
{
    /* 第一版只生成最基础的 PCM WAV：RIFF + fmt + data 三段。 */
    uint32_t byte_rate = APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint16_t block_align = APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint32_t riff_size = 36u + pcm_bytes;

    memcpy(&buf[0], "RIFF", 4u);
    app_ai_voice_wav_write_u32(&buf[4], riff_size);
    memcpy(&buf[8], "WAVEfmt ", 8u);
    app_ai_voice_wav_write_u32(&buf[16], 16u);
    app_ai_voice_wav_write_u16(&buf[20], 1u);
    app_ai_voice_wav_write_u16(&buf[22], APP_BUSINESS_AUDIO_CHANNELS);
    app_ai_voice_wav_write_u32(&buf[24], APP_BUSINESS_AUDIO_SAMPLE_RATE);
    app_ai_voice_wav_write_u32(&buf[28], byte_rate);
    app_ai_voice_wav_write_u16(&buf[32], block_align);
    app_ai_voice_wav_write_u16(&buf[34], APP_BUSINESS_AUDIO_BITS);
    memcpy(&buf[36], "data", 4u);
    app_ai_voice_wav_write_u32(&buf[40], pcm_bytes);
}

static int app_ai_voice_wav_parse_pcm(const uint8_t *wav,
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

    uint16_t audio_format = app_ai_voice_wav_read_u16(&wav[20]);
    uint16_t channels = app_ai_voice_wav_read_u16(&wav[22]);
    uint32_t sample_rate = app_ai_voice_wav_read_u32(&wav[24]);
    uint16_t bits = app_ai_voice_wav_read_u16(&wav[34]);

    /* 只接受和本机音频链路一致的 PCM 格式，避免错误播放噪声数据。 */
    if (audio_format != 1u || channels != APP_BUSINESS_AUDIO_CHANNELS ||
        sample_rate != APP_BUSINESS_AUDIO_SAMPLE_RATE || bits != APP_BUSINESS_AUDIO_BITS) {
        return -3;
    }

    uint32_t data_offset = 36u;
    while ((data_offset + 8u) <= wav_len) {
        uint32_t chunk_size = app_ai_voice_wav_read_u32(&wav[data_offset + 4u]);
        /* WAV 可能带有额外 chunk，按 chunk header 顺序查找 data。 */
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

static void app_ai_voice_task(void *arg)
{
    (void)arg;

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ai_recording || app_business_audio_session_try_begin() != 0) {
            s_ai_recording = 0;
            continue;
        }

        /* 长按期间按 20ms 帧读取 PCM，最多保留 2 秒，避免 HTTP body 超过模块限制。 */
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
            app_ai_voice_wav_write_header(s_ai_wav_buf, pcm_bytes);
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
                /* HTTP body 经 AT 口返回时可能混入前缀字节，先定位 RIFF 再解析。 */
                int riff_pos = app_ai_voice_find_riff(s_ai_wav_buf, resp_len);
                if (riff_pos > 0) {
                    memmove(s_ai_wav_buf, &s_ai_wav_buf[riff_pos], resp_len - (uint16_t)riff_pos);
                    resp_len = (uint16_t)(resp_len - (uint16_t)riff_pos);
                }
                ret = app_ai_voice_wav_parse_pcm(s_ai_wav_buf, resp_len, &resp_pcm, &resp_samples);
                if (ret == 0 && resp_samples > 0u) {
                    (void)service_audio_play(resp_pcm, resp_samples, 100u);
                } else {
                    APP_LOGW(TAG, "AI 响应 WAV 解析失败, ret=%d, len=%u", ret, (unsigned int)resp_len);
                }
            }
        }

        app_business_audio_session_end();
    }
}

int app_ai_voice_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = osal_task_create("biz_ai", app_ai_voice_task, NULL, 6144u, 5u, &s_ai_task);
    if (ret != 0) {
        APP_LOGE(TAG, "AI 语音任务启动失败, ret=%d", ret);
        return ret;
    }

    s_started = 1;
    return 0;
}

void app_ai_voice_record_start(void)
{
    if (app_business_audio_session_is_busy()) {
        return;
    }

    s_ai_recording = 1;
    if (s_ai_task != NULL) {
        (void)osal_task_notify_give(s_ai_task);
    }
}

void app_ai_voice_record_stop(void)
{
    s_ai_recording = 0;
}
