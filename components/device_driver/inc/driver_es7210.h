/**
 * @file driver_es7210.h
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
 * @brief ES7210 驱动初始化所需的 BSP 能力。
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
} driver_es7210_bsp_ops_t;

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

/**
 * @brief 初始化当前板级 ES7210 采集驱动。
 *
 * @param[in] ops BSP 能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int driver_es7210_init(const driver_es7210_bsp_ops_t *ops);

/**
 * @brief 释放当前板级 ES7210 采集驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_es7210_deinit(void);

/**
 * @brief 判断当前板级 ES7210 采集驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int driver_es7210_is_initialized(void);

/**
 * @brief 开始录音。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_es7210_start_record(void);

/**
 * @brief 停止录音。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_es7210_stop_record(void);

/**
 * @brief 读取单声道 PCM 样本。
 *
 * @param[out] pcm 单声道 PCM 输出缓冲区。
 * @param[in] samples 期望读取样本数。
 * @param[in] timeout_ms 读取超时时间，单位毫秒。
 * @return 实际读取样本数；失败返回负值。
 */
int driver_es7210_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ES7210_H */
