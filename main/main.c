/**
 * @file main.c
 * @brief 音频服务测试入口。
 */

#include "osal_log.h"
#include "osal_task.h"
#include "service_audio.h"

#include <stdint.h>

static const char *TAG = "audio_test";

#define AUDIO_TEST_MODE_PASSTHROUGH    0
#define AUDIO_TEST_MODE_READ_ONLY      1
#define AUDIO_TEST_MODE_PLAY_ONLY      2

/* 修改这里切换测试模式。默认本地说话直接播放。 */
#define AUDIO_TEST_MODE                AUDIO_TEST_MODE_PASSTHROUGH

#define AUDIO_TEST_FRAME_SAMPLES       256u
#define AUDIO_TEST_PLAY_AMPLITUDE      16000
#define AUDIO_TEST_PASSTHROUGH_GAIN    2u
#define AUDIO_TEST_LOG_INTERVAL        100u

static int16_t s_mono_buffer[AUDIO_TEST_FRAME_SAMPLES];
static int16_t s_tone_buffer[AUDIO_TEST_FRAME_SAMPLES];

static int32_t audio_test_calc_peak(const int16_t *data, uint32_t samples)
{
    int32_t peak = 0;

    for (uint32_t i = 0; i < samples; i++) {
        int32_t value = data[i];
        value = value < 0 ? -value : value;
        if (value > peak) {
            peak = value;
        }
    }

    return peak;
}

static void audio_test_make_tone(void)
{
    for (uint32_t i = 0; i < AUDIO_TEST_FRAME_SAMPLES; i++) {
        s_tone_buffer[i] = ((i / 8u) % 2u) ? AUDIO_TEST_PLAY_AMPLITUDE : -AUDIO_TEST_PLAY_AMPLITUDE;
    }
}

static int audio_test_init(void)
{
    service_audio_config_t cfg = {
        .volume = 100u,
        .passthrough_gain = AUDIO_TEST_PASSTHROUGH_GAIN,
        .input = SERVICE_AUDIO_INPUT_MIX_AVG,
    };

    int ret = service_audio_init(&cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "音频服务初始化失败, ret=%d", ret);
        return ret;
    }

    OSAL_LOGI(TAG, "音频服务测试初始化完成, input=MIX_AVG");
    return 0;
}

static void audio_test_read_only(void)
{
    uint32_t loop = 0;

    OSAL_LOGI(TAG, "开始 service_audio_read 单声道读取测试");

    while (1) {
        int ret = service_audio_read(s_mono_buffer, AUDIO_TEST_FRAME_SAMPLES, 1000u);
        if (ret > 0 && (loop % AUDIO_TEST_LOG_INTERVAL) == 0u) {
            int32_t peak = audio_test_calc_peak(s_mono_buffer, (uint32_t)ret);
            OSAL_LOGI(TAG,
                      "读取 mono PCM, loop=%u, samples=%d, peak=%ld",
                      (unsigned int)loop,
                      ret,
                      (long)peak);
        } else if (ret <= 0) {
            OSAL_LOGE(TAG, "service_audio_read 失败, ret=%d", ret);
        }

        loop++;
        osal_delay_ms(5u);
    }
}

static void audio_test_play_only(void)
{
    uint32_t loop = 0;

    audio_test_make_tone();
    OSAL_LOGI(TAG, "开始 service_audio_play 单声道播放测试");

    while (1) {
        int ret = service_audio_play(s_tone_buffer, AUDIO_TEST_FRAME_SAMPLES, 1000u);
        if (ret > 0 && (loop % AUDIO_TEST_LOG_INTERVAL) == 0u) {
            OSAL_LOGI(TAG, "播放 mono PCM, loop=%u, samples=%d", (unsigned int)loop, ret);
        } else if (ret <= 0) {
            OSAL_LOGE(TAG, "service_audio_play 失败, ret=%d", ret);
        }

        loop++;
        osal_delay_ms(1u);
    }
}

static void audio_test_passthrough(void)
{
    uint32_t loop = 0;

    OSAL_LOGI(TAG, "开始 service_audio 单声道本地直通测试");

    while (1) {
        int ret = service_audio_passthrough_once(s_mono_buffer, AUDIO_TEST_FRAME_SAMPLES, 1000u);
        if (ret <= 0) {
            OSAL_LOGE(TAG, "本地直通失败, ret=%d", ret);
        }

        if ((loop % AUDIO_TEST_LOG_INTERVAL) == 0u) {
            int32_t peak = audio_test_calc_peak(s_mono_buffer, AUDIO_TEST_FRAME_SAMPLES);
            OSAL_LOGI(TAG,
                      "本地直通, loop=%u, samples=%d, peak=%ld",
                      (unsigned int)loop,
                      ret,
                      (long)peak);
        }

        loop++;
        osal_delay_ms(1u);
    }
}

void app_main(void)
{
    OSAL_LOGI(TAG, "开始音频服务测试");

    if (audio_test_init() != 0) {
        return;
    }

    switch (AUDIO_TEST_MODE) {
    case AUDIO_TEST_MODE_READ_ONLY:
        audio_test_read_only();
        break;
    case AUDIO_TEST_MODE_PLAY_ONLY:
        audio_test_play_only();
        break;
    case AUDIO_TEST_MODE_PASSTHROUGH:
    default:
        audio_test_passthrough();
        break;
    }
}
