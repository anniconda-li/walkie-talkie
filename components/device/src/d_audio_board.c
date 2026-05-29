/**
 * @file d_audio_board.c
 * @brief WDRIVER 音频板级控制实现。
 */
#include "d_audio_board.h"

#include "d_config.h"
#include "driver/gpio.h"
#include "osal_task.h"

static const char *TAG = "d_audio";

static int d_audio_board_codec_set_enable_level(int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << d_audio_board_codec_ENABLE_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    int ret = gpio_config(&cfg);
    if (ret != 0) {
        D_LOGE(TAG, "音频 codec 使能脚配置失败, io=%d, ret=%d",
                 d_audio_board_codec_ENABLE_IO, ret);
        return ret < 0 ? ret : -ret;
    }

    ret = gpio_set_level(d_audio_board_codec_ENABLE_IO, level);
    if (ret != 0) {
        D_LOGE(TAG, "音频 codec 使能脚设置失败, io=%d, level=%d, ret=%d",
                 d_audio_board_codec_ENABLE_IO, level, ret);
        return ret < 0 ? ret : -ret;
    }

    D_LOGI(TAG, "音频 codec 使能脚已设置, io=%d, level=%d",
             d_audio_board_codec_ENABLE_IO, level);
    return 0;
}

int d_audio_board_codec_power_on(void)
{
    int ret = d_audio_board_codec_set_enable_level(1);
    if (ret == 0) {
        osal_delay_ms(100u);
    }

    return ret;
}

int d_audio_board_codec_power_off(void)
{
    return d_audio_board_codec_set_enable_level(0);
}
