/**
 * @file bsp_inmp441.h
 * @brief INMP441 I2S 数字麦克风驱动接口。
 *
 * INMP441 驱动只依赖底层提供的音频读取能力，不感知 I2S 控制器、引脚和通道句柄。
 */
#ifndef BSP_INMP441_H
#define BSP_INMP441_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief INMP441 驱动依赖的底层接口抽象。
 */
typedef struct {
    int (*read)(uint8_t *data,
                uint32_t len,
                uint32_t timeout_ms); /**< 从 I2S 读取麦克风数据函数。 */
} inmp441_interface_t;

/**
 * @brief INMP441 设备句柄。
 */
typedef struct inmp441_dev *inmp441_handle_t;

/**
 * @brief 初始化 INMP441 驱动对象。
 *
 * @param[in] itf 底层读取接口函数指针集合。
 * @return 成功返回 INMP441 句柄；失败返回 NULL。
 */
inmp441_handle_t inmp441_init(inmp441_interface_t *itf);

/**
 * @brief 释放 INMP441 驱动对象。
 *
 * @param[in] dev INMP441 句柄。
 */
void inmp441_deinit(inmp441_handle_t dev);

/**
 * @brief 读取 INMP441 麦克风音频数据。
 *
 * @param[in] dev INMP441 句柄。
 * @param[out] data 音频数据输出缓冲区。
 * @param[in] len 期望读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int inmp441_read(inmp441_handle_t dev,
                 uint8_t *data,
                 uint32_t len,
                 uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INMP441_H */
