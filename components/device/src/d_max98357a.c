/**
 * @file d_max98357a.c
 * @brief MAX98357A I2S 数字功放驱动实现。
 */
#include "d_max98357a.h"

#include "d_config.h"

#include <string.h>

static const char *TAG = "d_max98357a";

#define D_MAX98357A_MAX_FRAMES 256u

static d_max98357a_wdriver_ops_t s_d_ops;
static uint8_t s_d_inited = 0u;
static uint8_t s_d_volume = 80u;
static uint8_t s_d_mute = 0u;
static int16_t s_d_mono_buf[D_MAX98357A_MAX_FRAMES];

static int16_t d_max98357a_scale_sample(int16_t sample)
{
    if (s_d_mute || s_d_volume == 0u) {
        return 0;
    }

    int32_t scaled = ((int32_t)sample * (int32_t)s_d_volume) / 100;
    if (scaled > 32767) {
        return 32767;
    }
    if (scaled < -32768) {
        return -32768;
    }
    return (int16_t)scaled;
}

int d_max98357a_init(const d_max98357a_wdriver_ops_t *ops)
{
    if (s_d_inited) {
        return 0;
    }
    if (ops == NULL || ops->i2s_write == NULL) {
        D_LOGE(TAG, "MAX98357A 初始化失败: WDRIVER 能力无效");
        return -1;
    }

    s_d_ops = *ops;
    s_d_volume = 80u;
    s_d_mute = 0u;
    s_d_inited = 1u;
    D_LOGI(TAG, "MAX98357A 驱动初始化成功");
    return 0;
}

int d_max98357a_deinit(void)
{
    memset(&s_d_ops, 0, sizeof(s_d_ops));
    s_d_inited = 0u;
    s_d_volume = 80u;
    s_d_mute = 0u;
    return 0;
}

int d_max98357a_is_initialized(void)
{
    return s_d_inited ? 1 : 0;
}

int d_max98357a_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (!s_d_inited || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > D_MAX98357A_MAX_FRAMES) {
            frames = D_MAX98357A_MAX_FRAMES;
        }

        for (uint32_t i = 0; i < frames; i++) {
            s_d_mono_buf[i] = d_max98357a_scale_sample(pcm[total + i]);
        }

        uint32_t write_len = frames * sizeof(int16_t);
        int written = s_d_ops.i2s_write((const uint8_t *)s_d_mono_buf,
                                             write_len,
                                             timeout_ms);
        if (written < 0) {
            return written;
        }
        if (written == 0) {
            break;
        }

        total += (uint32_t)written / sizeof(int16_t);
        if ((uint32_t)written < write_len) {
            break;
        }
    }

    return total > 0u ? (int)total : 0;
}

int d_max98357a_set_volume(uint8_t volume)
{
    if (!s_d_inited) {
        return -1;
    }

    s_d_volume = volume > 100u ? 100u : volume;
    return 0;
}

int d_max98357a_set_mute(int mute)
{
    if (!s_d_inited) {
        return -1;
    }

    s_d_mute = mute ? 1u : 0u;
    return 0;
}
