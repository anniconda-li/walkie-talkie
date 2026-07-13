/**
 * @file app_audio_opus.c
 * @brief PTT 和 AI 请求录音共用的单例 Opus encoder。
 */
#include "app_audio_opus.h"

#include "app_config.h"
#include "osal_mutex.h"

#include "esp_heap_caps.h"
#include "esp_opus_enc.h"

#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "app_audio_opus";

/** @brief 全 App 唯一的常驻 Opus encoder handle。 */
static void *s_encoder = NULL;
static osal_mutex_t s_encoder_mutex = NULL;
static int s_input_bytes = 0;
static int s_output_bytes = 0;

static int app_audio_opus_lock(void)
{
    return s_encoder_mutex != NULL ?
           osal_mutex_lock(s_encoder_mutex, OSAL_WAIT_FOREVER) : -1;
}

static void app_audio_opus_unlock(void)
{
    if (s_encoder_mutex != NULL) {
        osal_mutex_unlock(s_encoder_mutex);
    }
}

int app_audio_opus_encoder_init(void)
{
    /* 业务启动顺序保证首次初始化发生在后台任务创建之前。 */
    if (s_encoder_mutex == NULL) {
        s_encoder_mutex = osal_mutex_create();
        if (s_encoder_mutex == NULL) {
            APP_LOGE(TAG, "audio_opus event=mutex_create_fail");
            return -1;
        }
    }

    if (app_audio_opus_lock() != 0) {
        return -2;
    }
    if (s_encoder != NULL) {
        app_audio_opus_unlock();
        return 0;
    }

    uint32_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.bitrate = APP_INTERCOM_OPUS_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 0;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = false;

    esp_audio_err_t codec_ret = esp_opus_enc_open(&cfg, sizeof(cfg), &s_encoder);
    if (codec_ret != ESP_AUDIO_ERR_OK || s_encoder == NULL) {
        APP_LOGE(TAG, "audio_opus event=encoder_open_fail ret=%d", (int)codec_ret);
        s_encoder = NULL;
        app_audio_opus_unlock();
        return -3;
    }

    codec_ret = esp_opus_enc_get_frame_size(s_encoder, &s_input_bytes, &s_output_bytes);
    if (codec_ret != ESP_AUDIO_ERR_OK ||
        s_input_bytes != (int)(APP_AUDIO_OPUS_FRAME_SAMPLES * sizeof(int16_t)) ||
        s_output_bytes <= 0 ||
        s_output_bytes > (int)APP_AUDIO_OPUS_PACKET_MAX_BYTES) {
        APP_LOGE(TAG,
                 "audio_opus event=frame_size_invalid ret=%d input=%d output=%d max=%u",
                 (int)codec_ret,
                 s_input_bytes,
                 s_output_bytes,
                 (unsigned int)APP_AUDIO_OPUS_PACKET_MAX_BYTES);
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
        s_input_bytes = 0;
        s_output_bytes = 0;
        app_audio_opus_unlock();
        return -4;
    }

    APP_LOGI(TAG,
             "intercom_opus_tx event=ready bitrate=%d frame_ms=20 input_bytes=%d output_bytes=%d "
             "internal_used=%u psram_used=%u payload_format=raw",
             APP_INTERCOM_OPUS_BITRATE,
             s_input_bytes,
             s_output_bytes,
             (unsigned int)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    app_audio_opus_unlock();
    return 0;
}

int app_audio_opus_encoder_reset(void)
{
    if (app_audio_opus_lock() != 0) {
        return -1;
    }
    int ret = s_encoder != NULL &&
              esp_opus_enc_reset(s_encoder) == ESP_AUDIO_ERR_OK ? 0 : -2;
    app_audio_opus_unlock();
    return ret;
}

int app_audio_opus_encode_20ms(const int16_t *pcm,
                               uint16_t samples,
                               uint8_t *output,
                               uint16_t output_capacity,
                               uint16_t *output_len)
{
    if (pcm == NULL || output == NULL || output_len == NULL ||
        samples != APP_AUDIO_OPUS_FRAME_SAMPLES ||
        output_capacity == 0u || output_capacity > APP_AUDIO_OPUS_PACKET_MAX_BYTES) {
        return -1;
    }
    *output_len = 0u;

    if (app_audio_opus_lock() != 0) {
        return -2;
    }
    if (s_encoder == NULL) {
        app_audio_opus_unlock();
        return -3;
    }

    esp_audio_enc_in_frame_t input = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)samples * sizeof(int16_t),
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = output,
        .len = output_capacity,
        .encoded_bytes = 0u,
        .pts = 0u,
    };
    esp_audio_err_t codec_ret = esp_opus_enc_process(s_encoder, &input, &encoded);
    if (codec_ret != ESP_AUDIO_ERR_OK ||
        encoded.encoded_bytes == 0u ||
        encoded.encoded_bytes > output_capacity ||
        encoded.encoded_bytes > APP_AUDIO_OPUS_PACKET_MAX_BYTES) {
        app_audio_opus_unlock();
        return -4;
    }

    *output_len = (uint16_t)encoded.encoded_bytes;
    app_audio_opus_unlock();
    return 0;
}
