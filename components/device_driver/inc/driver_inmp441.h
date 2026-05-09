/**
 * @file driver_inmp441.h
 * @brief INMP441 I2S 数字麦克风驱动接口。
 */
#ifndef DRIVER_INMP441_H
#define DRIVER_INMP441_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief INMP441 驱动初始化所需的 BSP 能力。
 */
typedef struct {
    int (*i2s_read)(uint8_t *data,
                    uint32_t len,
                    uint32_t timeout_ms); /**< I2S 读取能力。 */
} driver_inmp441_bsp_ops_t;

/**
 * @brief 初始化 INMP441 采集驱动。
 *
 * @param[in] ops BSP 能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int driver_inmp441_init(const driver_inmp441_bsp_ops_t *ops);

/**
 * @brief 释放 INMP441 采集驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_inmp441_deinit(void);

/**
 * @brief 判断 INMP441 采集驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int driver_inmp441_is_initialized(void);

/**
 * @brief 开始录音。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_inmp441_start_record(void);

/**
 * @brief 停止录音。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_inmp441_stop_record(void);

/**
 * @brief 读取单声道 PCM 样本。
 *
 * @param[out] pcm 单声道 PCM 输出缓冲区。
 * @param[in] samples 期望读取样本数。
 * @param[in] timeout_ms 读取超时时间，单位毫秒。
 * @return 实际读取样本数；失败返回负值。
 */
int driver_inmp441_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_INMP441_H */
