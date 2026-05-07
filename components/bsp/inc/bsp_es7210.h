/**
 * @file bsp_es7210.h
 * @brief ES7210 四通道音频 ADC 驱动接口。
 *
 * ES7210 驱动只依赖底层提供的 I2C 寄存器读写能力和 I2S 读取能力，
 * 不感知 I2C 总线、I2S 控制器、引脚或 ESP-IDF 句柄。
 */
#ifndef BSP_ES7210_H
#define BSP_ES7210_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ES7210 驱动依赖的底层能力集合。
 */
typedef struct {
    int (*write_reg)(uint8_t reg,
                     const uint8_t *data,
                     uint16_t len); /**< 写 ES7210 寄存器函数。 */
    int (*read_reg)(uint8_t reg,
                    uint8_t *data,
                    uint16_t len); /**< 读 ES7210 寄存器函数。 */
    int (*read)(uint8_t *data,
                uint32_t len,
                uint32_t timeout_ms); /**< 从 I2S 读取 ADC 音频数据函数。 */
} es7210_interface_t;

/**
 * @brief ES7210 设备句柄。
 */
typedef struct es7210_dev *es7210_handle_t;

/**
 * @brief 初始化 ES7210 驱动对象并配置芯片。
 *
 * @param[in] itf 底层能力集合。
 * @return 成功返回 ES7210 句柄；失败返回 NULL。
 */
es7210_handle_t es7210_init(es7210_interface_t *itf);

/**
 * @brief 释放 ES7210 驱动对象。
 *
 * @param[in] dev ES7210 句柄。
 */
void es7210_deinit(es7210_handle_t dev);

/**
 * @brief 读取 ES7210 音频数据。
 *
 * @param[in] dev ES7210 句柄。
 * @param[out] data 音频数据输出缓冲区。
 * @param[in] len 期望读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int es7210_read(es7210_handle_t dev,
                uint8_t *data,
                uint32_t len,
                uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ES7210_H */
