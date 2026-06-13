/**
 * @file main.c
 * @brief ES7210 + ES8311 audio loopback test entry.
 */

#include "d_init.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_init.h"
#include "wdriver_i2c.h"
#include "wdriver_i2s.h"

#include <stdint.h>

static const char *TAG = "main_audio_test";

#define MAIN_AUDIO_FRAME_SAMPLES 320u
#define MAIN_AUDIO_READ_TIMEOUT_MS 30u
#define MAIN_AUDIO_PLAY_TIMEOUT_MS 30u
#define MAIN_AUDIO_LOG_PERIOD 50u

static int16_t s_audio_frame[MAIN_AUDIO_FRAME_SAMPLES];

static void main_fatal(const char *stage, int ret)
{
    OSAL_LOGE(TAG, "%s失败, ret=%d", stage, ret);
    while (1) {
        osal_delay_ms(1000u);
    }
}

static int main_audio_peak_abs(const int16_t *pcm, uint32_t samples)
{
    int peak = 0;

    for (uint32_t i = 0; i < samples; i++) {
        int value = pcm[i];
        if (value < 0) {
            value = -value;
        }
        if (value > peak) {
            peak = value;
        }
    }

    return peak;
}

void app_main(void)
{
    OSAL_LOGI(TAG, "ES 音频回环测试开始");

    int ret = wdriver_i2c_init();
    if (ret != 0) {
        main_fatal("I2C 初始化", ret);
    }

    ret = wdriver_i2s_init();
    if (ret != 0) {
        main_fatal("I2S 初始化", ret);
    }

    ret = d_audio_init();
    if (ret != 0) {
        main_fatal("ES 音频驱动初始化", ret);
    }

    ret = service_init_audio();
    if (ret != 0) {
        main_fatal("音频服务初始化", ret);
    }

    ret = service_audio_set_volume(80u);
    if (ret != 0) {
        OSAL_LOGW(TAG, "设置音量失败, ret=%d", ret);
    }

    ret = service_audio_set_mute(0);
    if (ret != 0) {
        OSAL_LOGW(TAG, "取消静音失败, ret=%d", ret);
    }

    ret = service_audio_start_playback();
    if (ret != 0) {
        main_fatal("音频播放启动", ret);
    }

    OSAL_LOGI(TAG, "音频回环运行中, 每帧采样数=%u",
              (unsigned int)MAIN_AUDIO_FRAME_SAMPLES);

    uint32_t frame_count = 0u;
    while (1) {
        int read_samples = service_audio_read(s_audio_frame,
                                              MAIN_AUDIO_FRAME_SAMPLES,
                                              MAIN_AUDIO_READ_TIMEOUT_MS);
        if (read_samples < 0) {
            OSAL_LOGE(TAG, "音频读取失败, ret=%d", read_samples);
            osal_delay_ms(20u);
            continue;
        }
        if (read_samples == 0) {
            OSAL_LOGW(TAG, "音频读取超时");
            osal_delay_ms(5u);
            continue;
        }

        int played_samples = service_audio_play(s_audio_frame,
                                                (uint32_t)read_samples,
                                                MAIN_AUDIO_PLAY_TIMEOUT_MS);
        if (played_samples < 0) {
            OSAL_LOGE(TAG, "音频播放失败, ret=%d", played_samples);
            osal_delay_ms(20u);
            continue;
        }

        frame_count++;
        if ((frame_count % MAIN_AUDIO_LOG_PERIOD) == 0u) {
            OSAL_LOGI(TAG,
                      "音频帧=%u 读取=%d 播放=%d 峰值=%d",
                      (unsigned int)frame_count,
                      read_samples,
                      played_samples,
                      main_audio_peak_abs(s_audio_frame, (uint32_t)read_samples));
        }
    }
}
