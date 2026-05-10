/**
 * @file driver_inmp441.c
 * @brief INMP441 I2S 数字麦克风驱动实现。
 */
#include "driver_inmp441.h"

#include "driver_config.h"

#include <string.h>

static const char *TAG = "driver_inmp441";

#define DRIVER_INMP441_MAX_SAMPLES 256u

static driver_inmp441_bsp_ops_t s_driver_ops;
static uint8_t s_driver_inited = 0u;
static int16_t s_driver_raw_buf[DRIVER_INMP441_MAX_SAMPLES * 2u];

int driver_inmp441_init(const driver_inmp441_bsp_ops_t *ops)
{
    if (s_driver_inited) {
        return 0;
    }
    if (ops == NULL || ops->i2s_read == NULL) {
        DRIVER_LOGE(TAG, "INMP441 初始化失败: BSP 能力无效");
        return -1;
    }

    s_driver_ops = *ops;
    s_driver_inited = 1u;
    DRIVER_LOGI(TAG, "INMP441 驱动初始化成功");
    return 0;
}

int driver_inmp441_deinit(void)
{
    memset(&s_driver_ops, 0, sizeof(s_driver_ops));
    s_driver_inited = 0u;
    return 0;
}

int driver_inmp441_is_initialized(void)
{
    return s_driver_inited ? 1 : 0;
}

int driver_inmp441_start_record(void)
{
    return s_driver_inited ? 0 : -1;
}

int driver_inmp441_stop_record(void)
{
    return s_driver_inited ? 0 : -1;
}

int driver_inmp441_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (!s_driver_inited || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > DRIVER_INMP441_MAX_SAMPLES) {
            frames = DRIVER_INMP441_MAX_SAMPLES;
        }

        uint32_t read_len = frames * sizeof(int16_t) * 2u;
        int read_bytes = s_driver_ops.i2s_read((uint8_t *)s_driver_raw_buf,
                                               read_len,
                                               timeout_ms);
        if (read_bytes < 0) {
            return read_bytes;
        }
        if (read_bytes == 0) {
            break;
        }

        uint32_t read_frames = (uint32_t)read_bytes / (sizeof(int16_t) * 2u);
        for (uint32_t i = 0; i < read_frames; i++) {
            pcm[total + i] = s_driver_raw_buf[i * 2u];
        }
        total += read_frames;

        if ((uint32_t)read_bytes < read_len) {
            break;
        }
    }

    return total > 0u ? (int)total : 0;
}
