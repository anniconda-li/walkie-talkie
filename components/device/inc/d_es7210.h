/**
 * @file d_es7210.h
 * @brief ES7210 四通道音频 ADC 驱动接口。
 *
 * ES7210 驱动只依赖底层提供的 I2C 寄存器读写能力和 I2S 读取能力，
 * 不感知 I2C 总线、I2S 控制器、引脚或 ESP-IDF 句柄。
 */
#ifndef D_ES7210_H
#define D_ES7210_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ES7210 驱动初始化所需的 WDRIVER 能力。
 */
typedef struct {
    int (*i2c_write_reg)(uint16_t address,
                         uint32_t scl_speed_hz,
                         uint8_t reg,
                         const uint8_t *data,
                         uint16_t len); /**< I2C 寄存器写入能力。 */
    int (*i2c_read_reg)(uint16_t address,
                        uint32_t scl_speed_hz,
                        uint8_t reg,
                        uint8_t *data,
                        uint16_t len); /**< I2C 寄存器读取能力。 */
    int (*i2s_read)(uint8_t *data,
                    uint32_t len,
                    uint32_t timeout_ms); /**< I2S 读取能力。 */
} d_es7210_wdriver_ops_t;

/**
 * @brief 初始化当前板级 ES7210 采集驱动。
 *
 * @param[in] ops WDRIVER 能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int d_es7210_init(const d_es7210_wdriver_ops_t *ops);

/**
 * @brief 释放当前板级 ES7210 采集驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_es7210_deinit(void);

/**
 * @brief 判断当前板级 ES7210 采集驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int d_es7210_is_initialized(void);

/**
 * @brief 读取单声道 PCM 样本。
 *
 * @param[out] pcm 单声道 PCM 输出缓冲区。
 * @param[in] samples 期望读取样本数。
 * @param[in] timeout_ms 读取超时时间，单位毫秒。
 * @return 实际读取样本数；失败返回负值。
 */
int d_es7210_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* D_ES7210_H */
