/**
 * @file bsp_i2s.h
 * @brief BSP I2S 底层音频能力适配层。
 *
 * 本文件只暴露 I2S 初始化和面向具体音频器件的读写适配函数，
 * ES7210 与 ES8311 驱动无需感知 I2S 控制器、引脚或 ESP-IDF 句柄。
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
 * 函数内部完成 ES7210 录音 RX 通道和 ES8311 播放 TX 通道的创建、
 * 标准模式配置和使能。两个 codec 共用同一条 I2S 总线的 MCLK/BCLK/LRCK。
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
 * @brief ES7210 I2S 读取适配函数。
 *
 * @param[out] data 音频数据输出缓冲区。
 * @param[in] len 期望读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int es7210_i2s_read_impl(uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
 * @brief ES8311 I2S 播放适配函数。
 *
 * @param[in] data 待播放音频数据缓冲区。
 * @param[in] len 待写入长度，单位为字节。
 * @param[in] timeout_ms 写入超时时间，单位为毫秒。
 * @return 实际写入字节数；失败返回负值。
 */
int es8311_i2s_write_impl(const uint8_t *data, uint32_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2S_H */
