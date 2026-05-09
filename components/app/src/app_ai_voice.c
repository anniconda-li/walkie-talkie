/**
 * @file app_ai_voice.c
 * @brief AI 语音问答业务。
 */
#include "app_ai_voice.h"

#include "app_audio_session.h"
#include "app_business_config.h"
#include "app_common.h"
#include "app_wav.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "app_ai_voice";

static osal_task_t s_ai_task = NULL;
static volatile int s_started = 0;
static volatile int s_ai_recording = 0;
static union {
    uint8_t bytes[APP_BUSINESS_AI_WAV_MAX_BYTES];
    int16_t align;
} s_ai_wav_storage;

#define s_ai_wav_buf    (s_ai_wav_storage.bytes)

static void app_ai_voice_task(void *arg)
{
    (void)arg;

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ai_recording || app_audio_session_try_begin() != 0) {
            s_ai_recording = 0;
            continue;
        }

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
            app_wav_write_header(s_ai_wav_buf, pcm_bytes);
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
                int riff_pos = app_wav_find_riff(s_ai_wav_buf, resp_len);
                if (riff_pos > 0) {
                    memmove(s_ai_wav_buf, &s_ai_wav_buf[riff_pos], resp_len - (uint16_t)riff_pos);
                    resp_len = (uint16_t)(resp_len - (uint16_t)riff_pos);
                }
                ret = app_wav_parse_pcm16_mono_16k(s_ai_wav_buf, resp_len, &resp_pcm, &resp_samples);
                if (ret == 0 && resp_samples > 0u) {
                    (void)service_audio_play(resp_pcm, resp_samples, 100u);
                } else {
                    APP_LOGW(TAG, "AI 响应 WAV 解析失败, ret=%d, len=%u", ret, (unsigned int)resp_len);
                }
            }
        }

        app_audio_session_end();
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
    if (app_audio_session_is_busy()) {
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
