/**
 * @file driver_es8311.h
 * @brief ES8311 低功耗单声道音频 CODEC 驱动接口。
 *
 * ES8311 驱动只依赖底层提供的 I2C 寄存器读写能力和 I2S 写入能力，
 * 不感知 I2C 总线、I2S 控制器、引脚或 ESP-IDF 句柄。
 */
#ifndef BSP_ES8311_H
#define BSP_ES8311_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ES8311 驱动初始化所需的 BSP 能力。
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
    int (*i2s_write)(const uint8_t *data,
                     uint32_t len,
                     uint32_t timeout_ms); /**< I2S 写入能力。 */
} driver_es8311_bsp_ops_t;

/**
 * @brief 初始化当前板级 ES8311 播放驱动。
 *
 * @param[in] ops BSP 能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int driver_es8311_init(const driver_es8311_bsp_ops_t *ops);

/**
 * @brief 释放当前板级 ES8311 播放驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_es8311_deinit(void);

/**
 * @brief 判断当前板级 ES8311 播放驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int driver_es8311_is_initialized(void);

/**
 * @brief 播放单声道 PCM 样本。
 *
 * @param[in] pcm 单声道 PCM 输入缓冲区。
 * @param[in] samples 样本数。
 * @param[in] timeout_ms 写入超时时间，单位毫秒。
 * @return 实际播放样本数；失败返回负值。
 */
int driver_es8311_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

/**
 * @brief 设置当前板级 ES8311 播放音量。
 *
 * @param[in] volume 音量百分比。
 * @return 成功返回 0；失败返回负值。
 */
int driver_es8311_set_volume(uint8_t volume);

/**
 * @brief 设置当前板级 ES8311 静音状态。
 *
 * @param[in] mute 非 0 静音，0 取消静音。
 * @return 成功返回 0；失败返回负值。
 */
int driver_es8311_set_mute(int mute);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ES8311_H */
