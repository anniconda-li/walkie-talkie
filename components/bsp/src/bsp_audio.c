/**
 * @file bsp_audio.c
 * @brief BSP 音频板级控制实现。
 */
#include "bsp_audio.h"

#include "bsp_common.h"
#include "driver/gpio.h"
#include "osal_task.h"

static const char *TAG = "bsp_audio";

static int bsp_audio_codec_set_enable_level(int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_AUDIO_CODEC_ENABLE_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    int ret = gpio_config(&cfg);
    if (ret != 0) {
        BSP_LOGE(TAG, "音频 codec 使能脚配置失败, io=%d, ret=%d",
                 BSP_AUDIO_CODEC_ENABLE_IO, ret);
        return ret < 0 ? ret : -ret;
    }

    ret = gpio_set_level(BSP_AUDIO_CODEC_ENABLE_IO, level);
    if (ret != 0) {
        BSP_LOGE(TAG, "音频 codec 使能脚设置失败, io=%d, level=%d, ret=%d",
                 BSP_AUDIO_CODEC_ENABLE_IO, level, ret);
        return ret < 0 ? ret : -ret;
    }

    BSP_LOGI(TAG, "音频 codec 使能脚已设置, io=%d, level=%d",
             BSP_AUDIO_CODEC_ENABLE_IO, level);
    return 0;
}

int bsp_audio_codec_power_on(void)
{
    int ret = bsp_audio_codec_set_enable_level(1);
    if (ret == 0) {
        osal_delay_ms(100u);
    }

    return ret;
}

int bsp_audio_codec_power_off(void)
{
    return bsp_audio_codec_set_enable_level(0);
}
