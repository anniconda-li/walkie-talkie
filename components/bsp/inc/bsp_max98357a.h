/**
 * @file bsp_max98357a.h
 * @brief MAX98357A I2S 数字功放驱动接口。
 *
 * MAX98357A 驱动只依赖底层提供的音频写入能力，不感知 I2S 控制器、引脚和通道句柄。
 */
#ifndef BSP_MAX98357A_H
#define BSP_MAX98357A_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MAX98357A 驱动依赖的底层接口抽象。
 */
typedef struct {
    int (*write)(const uint8_t *data,
                 uint32_t len,
                 uint32_t timeout_ms); /**< 向 I2S 写入播放数据函数。 */
} max98357a_interface_t;

/**
 * @brief MAX98357A 设备句柄。
 */
typedef struct max98357a_dev *max98357a_handle_t;

/**
 * @brief 初始化 MAX98357A 驱动对象。
 *
 * @param[in] itf 底层写入接口函数指针集合。
 * @return 成功返回 MAX98357A 句柄；失败返回 NULL。
 */
max98357a_handle_t max98357a_init(max98357a_interface_t *itf);

/**
 * @brief 释放 MAX98357A 驱动对象。
 *
 * @param[in] dev MAX98357A 句柄。
 */
void max98357a_deinit(max98357a_handle_t dev);

/**
 * @brief 播放 MAX98357A 音频数据。
 *
 * @param[in] dev MAX98357A 句柄。
 * @param[in] data 待播放音频数据缓冲区。
 * @param[in] len 待播放数据长度，单位为字节。
 * @param[in] timeout_ms 写入超时时间，单位为毫秒。
 * @return 实际写入字节数；失败返回负值。
 */
int max98357a_play(max98357a_handle_t dev,
                   const uint8_t *data,
                   uint32_t len,
                   uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_MAX98357A_H */
