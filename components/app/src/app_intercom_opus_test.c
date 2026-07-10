/**
 * @file app_intercom_opus_test.c
 * @brief 按住 PTT 采集并编码到 PSRAM，松手后本地解码播放。
 */
#include "app_intercom_opus_test.h"

#include "app_business.h"
#include "app_config.h"
#include "app_ui.h"
#include "osal_heap.h"
#include "osal_task.h"
#include "service_audio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#if APP_INTERCOM_OPUS_LOCAL_TEST_ENABLE

static const char *TAG = "intercom_opus_test";

#define APP_INTERCOM_OPUS_TEST_TASK_STACK       40960u
#define APP_INTERCOM_OPUS_PACKET_MAX_BYTES      1275u
#define APP_INTERCOM_OPUS_TEST_STORE_BYTES      (128u * 1024u)
#define APP_INTERCOM_OPUS_TEST_LEN_BYTES        2u

static osal_task_t s_task = NULL;
static volatile int s_active = 0;
static void *s_encoder = NULL;
static void *s_decoder = NULL;
static uint8_t *s_store = NULL;
static int s_input_bytes = 0;
static int s_output_bytes = 0;

static void app_intercom_opus_test_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t app_intercom_opus_test_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static int app_intercom_opus_test_init_codec(void)
{
    if (s_encoder != NULL && s_decoder != NULL && s_store != NULL) {
        return 0;
    }

    uint32_t internal_before = osal_heap_get_internal_free_size();
    uint32_t psram_before = osal_heap_get_external_free_size();
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = APP_INTERCOM_OPUS_LOCAL_TEST_BITRATE;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 0;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;

    esp_audio_err_t codec_ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_encoder);
    if (codec_ret != ESP_AUDIO_ERR_OK || s_encoder == NULL) {
        APP_LOGE(TAG, "event=encoder_open_fail ret=%d", (int)codec_ret);
        s_encoder = NULL;
        return -1;
    }

    codec_ret = esp_opus_enc_get_frame_size(s_encoder, &s_input_bytes, &s_output_bytes);
    if (codec_ret != ESP_AUDIO_ERR_OK ||
        s_input_bytes != (int)APP_BUSINESS_FRAME_BYTES ||
        s_output_bytes <= 0 ||
        s_output_bytes > (int)APP_INTERCOM_OPUS_PACKET_MAX_BYTES) {
        APP_LOGE(TAG,
                 "event=frame_size_invalid ret=%d input=%d output=%d expected_input=%u",
                 (int)codec_ret,
                 s_input_bytes,
                 s_output_bytes,
                 (unsigned int)APP_BUSINESS_FRAME_BYTES);
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
        return -2;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    dec_cfg.self_delimited = false;
    codec_ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_decoder);
    if (codec_ret != ESP_AUDIO_ERR_OK || s_decoder == NULL) {
        APP_LOGE(TAG, "event=decoder_open_fail ret=%d", (int)codec_ret);
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
        s_decoder = NULL;
        return -3;
    }

    s_store = (uint8_t *)osal_heap_alloc_external(APP_INTERCOM_OPUS_TEST_STORE_BYTES);
    if (s_store == NULL) {
        APP_LOGE(TAG,
                 "event=store_alloc_fail bytes=%u psram_free=%u",
                 (unsigned int)APP_INTERCOM_OPUS_TEST_STORE_BYTES,
                 (unsigned int)osal_heap_get_external_free_size());
        (void)esp_opus_dec_close(s_decoder);
        esp_opus_enc_close(s_encoder);
        s_decoder = NULL;
        s_encoder = NULL;
        return -4;
    }

    APP_LOGI(TAG,
             "event=ready rate=%u channels=%u frame_ms=20 bitrate=%d input_bytes=%d "
             "output_bytes=%d store_bytes=%u internal_used=%u psram_used=%u",
             (unsigned int)APP_BUSINESS_AUDIO_SAMPLE_RATE,
             (unsigned int)APP_BUSINESS_AUDIO_CHANNELS,
             APP_INTERCOM_OPUS_LOCAL_TEST_BITRATE,
             s_input_bytes,
             s_output_bytes,
             (unsigned int)APP_INTERCOM_OPUS_TEST_STORE_BYTES,
             (unsigned int)(internal_before - osal_heap_get_internal_free_size()),
             (unsigned int)(psram_before - osal_heap_get_external_free_size()));
    return 0;
}

static void app_intercom_opus_test_run(void)
{
    if (s_encoder == NULL || s_decoder == NULL || s_store == NULL) {
        (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_FAILED);
        return;
    }
    if (app_business_audio_session_try_begin() != 0) {
        APP_LOGW(TAG, "event=record_ignored reason=audio_busy");
        return;
    }
    if (esp_opus_enc_reset(s_encoder) != ESP_AUDIO_ERR_OK) {
        APP_LOGE(TAG, "event=encoder_reset_fail");
        (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_FAILED);
        app_business_audio_session_end();
        return;
    }

    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES] __attribute__((aligned(16)));
    uint8_t opus_packet[APP_INTERCOM_OPUS_PACKET_MAX_BYTES] __attribute__((aligned(16)));
    size_t store_used = 0u;
    uint32_t frames = 0u;
    uint32_t encoded_bytes = 0u;
    uint32_t read_fail = 0u;
    uint32_t encode_fail = 0u;
    uint32_t encode_calls = 0u;
    uint32_t encode_max_us = 0u;
    uint64_t encode_total_us = 0u;
    uint8_t store_full = 0u;
    uint32_t record_start_ms = osal_get_tick_ms();

    (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_TALKING);
    APP_LOGI(TAG,
             "event=record_start max_ms=%u bitrate=%d",
             (unsigned int)APP_INTERCOM_OPUS_LOCAL_TEST_MAX_MS,
             APP_INTERCOM_OPUS_LOCAL_TEST_BITRATE);

    while (s_active &&
           (uint32_t)(osal_get_tick_ms() - record_start_ms) < APP_INTERCOM_OPUS_LOCAL_TEST_MAX_MS) {
        int samples = service_audio_read(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
        if (samples != (int)APP_BUSINESS_FRAME_SAMPLES) {
            read_fail++;
            osal_delay_ms(5u);
            continue;
        }

        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)pcm,
            .len = APP_BUSINESS_FRAME_BYTES,
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = opus_packet,
            .len = sizeof(opus_packet),
            .encoded_bytes = 0u,
            .pts = 0u,
        };
        int64_t encode_start_us = esp_timer_get_time();
        esp_audio_err_t codec_ret = esp_opus_enc_process(s_encoder, &input, &output);
        uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_start_us);
        encode_calls++;
        encode_total_us += encode_us;
        if (encode_us > encode_max_us) {
            encode_max_us = encode_us;
        }
        if (codec_ret != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes == 0u ||
            output.encoded_bytes > APP_INTERCOM_OPUS_PACKET_MAX_BYTES) {
            encode_fail++;
            continue;
        }

        size_t needed = APP_INTERCOM_OPUS_TEST_LEN_BYTES + output.encoded_bytes;
        if (store_used + needed > APP_INTERCOM_OPUS_TEST_STORE_BYTES) {
            store_full = 1u;
            break;
        }
        app_intercom_opus_test_write_u16(&s_store[store_used],
                                         (uint16_t)output.encoded_bytes);
        memcpy(&s_store[store_used + APP_INTERCOM_OPUS_TEST_LEN_BYTES],
               opus_packet,
               output.encoded_bytes);
        store_used += needed;
        encoded_bytes += output.encoded_bytes;
        frames++;
    }

    uint32_t capture_ms = (uint32_t)(osal_get_tick_ms() - record_start_ms);
    while (s_active) {
        osal_delay_ms(10u);
    }

    APP_LOGI(TAG,
             "event=record_stop dur_ms=%u frames=%u bytes=%u read_fail=%u encode_fail=%u "
             "encode_avg_us=%u encode_max_us=%u store_full=%u",
             (unsigned int)capture_ms,
             (unsigned int)frames,
             (unsigned int)encoded_bytes,
             (unsigned int)read_fail,
             (unsigned int)encode_fail,
             encode_calls > 0u ? (unsigned int)(encode_total_us / encode_calls) : 0u,
             (unsigned int)encode_max_us,
             (unsigned int)store_full);

    if (frames == 0u || esp_opus_dec_reset(s_decoder) != ESP_AUDIO_ERR_OK) {
        APP_LOGE(TAG, "event=play_abort frames=%u", (unsigned int)frames);
        (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_FAILED);
        app_business_audio_session_end();
        return;
    }
    if (service_audio_start_playback() != 0) {
        APP_LOGE(TAG, "event=play_start_fail");
        (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_FAILED);
        app_business_audio_session_end();
        return;
    }

    (void)app_ui_set_intercom_state(APP_UI_INTERCOM_STATE_LOCAL_PLAYBACK);
    size_t store_pos = 0u;
    uint32_t decoded_frames = 0u;
    uint32_t decode_fail = 0u;
    uint32_t decode_calls = 0u;
    uint32_t play_fail = 0u;
    uint32_t decode_max_us = 0u;
    uint32_t play_max_ms = 0u;
    uint64_t decode_total_us = 0u;
    APP_LOGI(TAG, "event=play_start frames=%u", (unsigned int)frames);

    while (store_pos + APP_INTERCOM_OPUS_TEST_LEN_BYTES <= store_used) {
        uint16_t packet_len = app_intercom_opus_test_read_u16(&s_store[store_pos]);
        store_pos += APP_INTERCOM_OPUS_TEST_LEN_BYTES;
        if (packet_len == 0u ||
            packet_len > APP_INTERCOM_OPUS_PACKET_MAX_BYTES ||
            store_pos + packet_len > store_used) {
            decode_fail++;
            break;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = &s_store[store_pos],
            .len = packet_len,
            .consumed = 0u,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t decoded = {
            .buffer = (uint8_t *)pcm,
            .len = sizeof(pcm),
            .needed_size = 0u,
            .decoded_size = 0u,
        };
        esp_audio_dec_info_t info = {0};
        int64_t decode_start_us = esp_timer_get_time();
        esp_audio_err_t codec_ret = esp_opus_dec_decode(s_decoder, &raw, &decoded, &info);
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start_us);
        decode_calls++;
        decode_total_us += decode_us;
        if (decode_us > decode_max_us) {
            decode_max_us = decode_us;
        }
        store_pos += packet_len;
        if (codec_ret != ESP_AUDIO_ERR_OK ||
            raw.consumed != packet_len ||
            decoded.decoded_size != APP_BUSINESS_FRAME_BYTES) {
            decode_fail++;
            osal_delay_ms(1u);
            continue;
        }

        uint32_t play_start_ms = osal_get_tick_ms();
        int played = service_audio_play(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
        uint32_t play_ms = (uint32_t)(osal_get_tick_ms() - play_start_ms);
        if (play_ms > play_max_ms) {
            play_max_ms = play_ms;
        }
        if (played != (int)APP_BUSINESS_FRAME_SAMPLES) {
            play_fail++;
            break;
        }
        decoded_frames++;
    }

    (void)service_audio_stop_playback();
    APP_LOGI(TAG,
             "event=play_stop frames=%u decode_fail=%u play_fail=%u decode_avg_us=%u "
             "decode_max_us=%u play_max_ms=%u stack_hwm=%u internal_free=%u psram_free=%u",
             (unsigned int)decoded_frames,
             (unsigned int)decode_fail,
             (unsigned int)play_fail,
             decode_calls > 0u ? (unsigned int)(decode_total_us / decode_calls) : 0u,
             (unsigned int)decode_max_us,
             (unsigned int)play_max_ms,
             (unsigned int)uxTaskGetStackHighWaterMark(NULL),
             (unsigned int)osal_heap_get_internal_free_size(),
             (unsigned int)osal_heap_get_external_free_size());
    (void)app_ui_set_intercom_state((decode_fail == 0u && play_fail == 0u) ?
                                    APP_UI_INTERCOM_STATE_IDLE :
                                    APP_UI_INTERCOM_STATE_FAILED);
    app_business_audio_session_end();
}

static void app_intercom_opus_test_task(void *arg)
{
    (void)arg;
    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (s_active) {
            app_intercom_opus_test_run();
        }
    }
}

int app_intercom_opus_test_start(void)
{
    if (s_task != NULL) {
        return 0;
    }
    int ret = app_intercom_opus_test_init_codec();
    if (ret != 0) {
        return ret;
    }
    TaskHandle_t handle = NULL;
    BaseType_t task_ret = xTaskCreateWithCaps(app_intercom_opus_test_task,
                                              "opus_local",
                                              APP_INTERCOM_OPUS_TEST_TASK_STACK,
                                              NULL,
                                              6u,
                                              &handle,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        APP_LOGE(TAG,
                 "event=task_start_fail ret=%d stack=%u psram_free=%u internal_free=%u internal_largest=%u",
                 (int)task_ret,
                 (unsigned int)APP_INTERCOM_OPUS_TEST_TASK_STACK,
                 (unsigned int)osal_heap_get_external_free_size(),
                 (unsigned int)osal_heap_get_internal_free_size(),
                 (unsigned int)osal_heap_get_internal_largest_free_block());
        return -1;
    }
    s_task = (osal_task_t)handle;
    APP_LOGI(TAG, "event=enabled websocket=disabled hold_ptt=record release_ptt=playback");
    return 0;
}

void app_intercom_opus_test_ptt_start(void)
{
    if (s_task == NULL || app_business_audio_session_is_busy()) {
        APP_LOGW(TAG, "event=record_ignored reason=not_ready_or_audio_busy");
        return;
    }
    s_active = 1;
    (void)osal_task_notify_give(s_task);
}

void app_intercom_opus_test_ptt_stop(void)
{
    s_active = 0;
}

#else

int app_intercom_opus_test_start(void)
{
    return -1;
}

void app_intercom_opus_test_ptt_start(void)
{
}

void app_intercom_opus_test_ptt_stop(void)
{
}

#endif
