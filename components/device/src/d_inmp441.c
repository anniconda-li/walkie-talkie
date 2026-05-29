/**
 * @file d_inmp441.c
 * @brief INMP441 I2S 数字麦克风驱动实现。
 */
#include "d_inmp441.h"

#include "d_config.h"

#include <string.h>

static const char *TAG = "d_inmp441";

#define D_INMP441_MAX_SAMPLES 256u

/** @brief 初始化时承接并保存的 WDRIVER 能力函数表。 */
static d_inmp441_wdriver_ops_t s_d_ops;

/** @brief INMP441 驱动是否已完成初始化。 */
static uint8_t s_d_inited = 0u;

/** @brief INMP441 单次 I2S 读取的临时 PCM 缓存。 */
static int16_t s_d_raw_buf[D_INMP441_MAX_SAMPLES];

int d_inmp441_init(const d_inmp441_wdriver_ops_t *ops)
{
    if (s_d_inited) {
        return 0;
    }
    if (ops == NULL || ops->i2s_read == NULL) {
        D_LOGE(TAG, "INMP441 初始化失败: WDRIVER 能力无效");
        return -1;
    }

    s_d_ops = *ops;
    s_d_inited = 1u;
    D_LOGI(TAG, "INMP441 驱动初始化成功");
    return 0;
}

int d_inmp441_deinit(void)
{
    memset(&s_d_ops, 0, sizeof(s_d_ops));
    s_d_inited = 0u;
    return 0;
}

int d_inmp441_is_initialized(void)
{
    return s_d_inited ? 1 : 0;
}

int d_inmp441_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (!s_d_inited || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > D_INMP441_MAX_SAMPLES) {
            frames = D_INMP441_MAX_SAMPLES;
        }

        uint32_t read_len = frames * sizeof(int16_t);
        int read_bytes = s_d_ops.i2s_read((uint8_t *)s_d_raw_buf,
                                               read_len,
                                               timeout_ms);
        if (read_bytes < 0) {
            return read_bytes;
        }
        if (read_bytes == 0) {
            break;
        }

        uint32_t read_frames = (uint32_t)read_bytes / sizeof(int16_t);
        for (uint32_t i = 0; i < read_frames; i++) {
            pcm[total + i] = s_d_raw_buf[i];
        }
        total += read_frames;

        if ((uint32_t)read_bytes < read_len) {
            break;
        }
    }

    return total > 0u ? (int)total : 0;
}
