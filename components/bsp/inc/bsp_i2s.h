/**
 * @file bsp_i2s.h
 * @brief BSP I2S 底层音频能力适配层。
 *
 * 本文件只暴露 I2S 初始化和面向具体音频器件的读写适配函数，
 * INMP441 与 MAX98357A 驱动无需感知 I2S 控制器、引脚或 ESP-IDF 句柄。
 */
#ifndef BSP_I2S_H
#define BSP_I2S_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化项目使用的 I2S RX/TX 通道。
 *
 * 函数内部完成 INMP441 麦克风 RX 通道和 MAX98357A 功放 TX 通道的创建、
 * 标准模式配置和使能。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_i2s_init(void);

/**
 * @brief 释放项目使用的 I2S RX/TX 通道。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_i2s_deinit(void);

/**
 * @brief INMP441 I2S 读取适配函数。
 *
 * @param[out] data 音频数据输出缓冲区。
 * @param[in] len 期望读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int inmp441_i2s_read_impl(uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
 * @brief MAX98357A I2S 播放适配函数。
 *
 * @param[in] data 待播放音频数据缓冲区。
 * @param[in] len 待写入长度，单位为字节。
 * @param[in] timeout_ms 写入超时时间，单位为毫秒。
 * @return 实际写入字节数；失败返回负值。
 */
int max98357a_i2s_write_impl(const uint8_t *data, uint32_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2S_H */
