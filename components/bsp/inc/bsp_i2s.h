/**
 * @file bsp_i2s.h
 * @brief BSP I2S bus adapter.
 *
 * 本文件只暴露 I2S 初始化和通用读写适配函数，具体音频器件由 driver 层组合。
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
 * 函数内部完成项目音频 RX/TX 通道的创建、标准模式配置和使能。
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
 * @brief Read bytes from the board I2S RX channel.
 *
 * @param[out] data 音频数据输出缓冲区。
 * @param[in] len 期望读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int bsp_i2s_read(uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
 * @brief Write bytes to the board I2S TX channel.
 *
 * @param[in] data 待播放音频数据缓冲区。
 * @param[in] len 待写入长度，单位为字节。
 * @param[in] timeout_ms 写入超时时间，单位为毫秒。
 * @return 实际写入字节数；失败返回负值。
 */
int bsp_i2s_write(const uint8_t *data, uint32_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2S_H */
