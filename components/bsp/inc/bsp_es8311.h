/**
 * @file bsp_es8311.h
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
 * @brief ES8311 驱动依赖的底层能力集合。
 */
typedef struct {
    int (*write_reg)(uint8_t reg,
                     const uint8_t *data,
                     uint16_t len); /**< 写 ES8311 寄存器函数。 */
    int (*read_reg)(uint8_t reg,
                    uint8_t *data,
                    uint16_t len); /**< 读 ES8311 寄存器函数。 */
    int (*write)(const uint8_t *data,
                 uint32_t len,
                 uint32_t timeout_ms); /**< 向 I2S 写入播放数据函数。 */
} es8311_interface_t;

/**
 * @brief ES8311 设备句柄。
 */
typedef struct es8311_dev *es8311_handle_t;

/**
 * @brief 初始化 ES8311 驱动对象并配置芯片。
 *
 * @param[in] itf 底层能力集合。
 * @return 成功返回 ES8311 句柄；失败返回 NULL。
 */
es8311_handle_t es8311_init(es8311_interface_t *itf);

/**
 * @brief 释放 ES8311 驱动对象。
 *
 * @param[in] dev ES8311 句柄。
 */
void es8311_deinit(es8311_handle_t dev);

/**
 * @brief 播放 ES8311 音频数据。
 *
 * @param[in] dev ES8311 句柄。
 * @param[in] data 待播放音频数据缓冲区。
 * @param[in] len 待播放数据长度，单位为字节。
 * @param[in] timeout_ms 写入超时时间，单位为毫秒。
 * @return 实际写入字节数；失败返回负值。
 */
int es8311_play(es8311_handle_t dev,
                const uint8_t *data,
                uint32_t len,
                uint32_t timeout_ms);

/**
 * @brief 设置 ES8311 DAC 音量。
 *
 * @param[in] dev ES8311 句柄。
 * @param[in] volume 音量百分比，范围 0 到 100。
 * @return 成功返回 0；失败返回负值。
 */
int es8311_set_volume(es8311_handle_t dev, uint8_t volume);

/**
 * @brief 设置 ES8311 静音状态。
 *
 * @param[in] dev ES8311 句柄。
 * @param[in] mute 非 0 表示静音，0 表示取消静音。
 * @return 成功返回 0；失败返回负值。
 */
int es8311_set_mute(es8311_handle_t dev, int mute);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ES8311_H */
