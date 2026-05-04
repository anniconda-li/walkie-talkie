/**
 * @file bsp_i2c.h
 * @brief BSP I2C 主机总线驱动抽象层。
 *
 * 对外提供项目内统一的 I2C 初始化入口，以及面向具体器件驱动的 I2C 适配函数。
 * 总线配置、设备地址和底层设备句柄均封装在 BSP I2C 内部。
 */
#ifndef BSP_I2C_H
#define BSP_I2C_H

#include <stdint.h>

#include "bsp_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 默认 I2C 控制器端口。
 */
#ifndef BSP_I2C_PORT
#define BSP_I2C_PORT 0
#endif

/**
 * @brief 默认 I2C SDA 引脚。
 *
 * 如实际硬件不同，可在包含本头文件前或编译选项中重定义该宏。
 */
#ifndef BSP_I2C_SDA_IO
#define BSP_I2C_SDA_IO GPIO_NUM_1
#endif

/**
 * @brief 默认 I2C SCL 引脚。
 *
 * 如实际硬件不同，可在包含本头文件前或编译选项中重定义该宏。
 */
#ifndef BSP_I2C_SCL_IO
#define BSP_I2C_SCL_IO GPIO_NUM_2
#endif

/**
 * @brief 默认 I2C SCL 频率。
 */
#ifndef BSP_I2C_SCL_SPEED_HZ
#define BSP_I2C_SCL_SPEED_HZ 100000u
#endif

/**
 * @brief PCA9557 使用的 I2C 端口。
 */
#ifndef I2C_PORT_PCA9557
#define I2C_PORT_PCA9557 BSP_I2C_PORT
#endif

/**
 * @brief PCA9557 使用的 I2C 地址。
 */
#ifndef I2C_ADDR_PCA9557
#define I2C_ADDR_PCA9557 0x18u
#endif

/**
 * @brief PCA9557 使用的 I2C SCL 频率。
 */
#ifndef I2C_PCA9557_SCL_SPEED_HZ
#define I2C_PCA9557_SCL_SPEED_HZ BSP_I2C_SCL_SPEED_HZ
#endif

/**
 * @brief 默认 I2C 传输超时时间。
 */
#ifndef BSP_I2C_XFER_TIMEOUT_MS
#define BSP_I2C_XFER_TIMEOUT_MS 1000
#endif

/**
 * @brief 初始化 I2C 总线并挂载项目内 I2C 设备。
 *
 * @return 成功返回 0；失败返回负值。
 * @note 当前会初始化 PCA9557 所在 I2C 总线，并添加 PCA9557 设备。
 */
int bsp_i2c_init(void);

/**
 * @brief 释放 I2C 主机总线。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_i2c_deinit(void);

/**
 * @brief 获取 BSP I2C 总线句柄。
 *
 * @return 已初始化的 I2C 总线句柄；未初始化时返回 NULL。
 * @note 该接口只供 BSP 内部适配 ESP-IDF 组件使用，设备驱动不应直接依赖该句柄。
 */
void *bsp_i2c_get_bus_handle(void);

/**
 * @brief PCA9557 I2C 寄存器写适配函数。
 *
 * @param[in] reg 寄存器地址。
 * @param[in] data 待写入数据。
 * @param[in] len 待写入数据长度，单位为字节。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_i2c_write_reg_impl(uint8_t reg,
                               const uint8_t *data,
                               uint16_t len);

/**
 * @brief PCA9557 I2C 寄存器读适配函数。
 *
 * @param[in] reg 寄存器地址。
 * @param[out] data 读取数据输出缓冲区。
 * @param[in] len 读取数据长度，单位为字节。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_i2c_read_reg_impl(uint8_t reg,
                              uint8_t *data,
                              uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2C_H */
