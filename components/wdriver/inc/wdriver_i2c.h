/**
 * @file wdriver_i2c.h
 * @brief WDRIVER I2C 总线适配接口。
 */
#ifndef WDRIVER_I2C_H
#define WDRIVER_I2C_H

#include <stdint.h>

#include "wdriver_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 板级 I2C 控制器端口号。
 */
#ifndef WDRIVER_I2C_PORT
#define WDRIVER_I2C_PORT 0
#endif

/**
 * @brief 板级 I2C SDA 引脚。
 */
#ifndef WDRIVER_I2C_SDA_IO
#define WDRIVER_I2C_SDA_IO GPIO_NUM_1
#endif

/**
 * @brief 板级 I2C SCL 引脚。
 */
#ifndef WDRIVER_I2C_SCL_IO
#define WDRIVER_I2C_SCL_IO GPIO_NUM_2
#endif

/**
 * @brief 板级 I2C 默认时钟频率，单位为 Hz。
 */
#ifndef WDRIVER_I2C_SCL_SPEED_HZ
#define WDRIVER_I2C_SCL_SPEED_HZ 100000u
#endif

/**
 * @brief 板级 I2C 单次传输超时时间，单位为毫秒。
 */
#ifndef WDRIVER_I2C_XFER_TIMEOUT_MS
#define WDRIVER_I2C_XFER_TIMEOUT_MS 1000
#endif

/**
 * @brief Initialize the board I2C bus.
 *
 * @return 0 on success, negative value on failure.
 */
int wdriver_i2c_init(void);

/**
 * @brief Deinitialize the board I2C bus.
 *
 * @return 0 on success, negative value on failure.
 */
int wdriver_i2c_deinit(void);

/**
 * @brief Get the native ESP-IDF I2C bus handle.
 *
 * @return Bus handle, or NULL if the bus is not initialized.
 */
void *wdriver_i2c_get_bus_handle(void);

/**
 * @brief Write bytes to an I2C device register.
 *
 * @param[in] address 7-bit I2C address.
 * @param[in] scl_speed_hz Device bus speed.
 * @param[in] reg Register address.
 * @param[in] data Data to write.
 * @param[in] len Data length in bytes.
 * @return 0 on success, negative value on failure.
 */
int wdriver_i2c_write_reg(uint16_t address,
                      uint32_t scl_speed_hz,
                      uint8_t reg,
                      const uint8_t *data,
                      uint16_t len);

/**
 * @brief Read bytes from an I2C device register.
 *
 * @param[in] address 7-bit I2C address.
 * @param[in] scl_speed_hz Device bus speed.
 * @param[in] reg Register address.
 * @param[out] data Output buffer.
 * @param[in] len Read length in bytes.
 * @return 0 on success, negative value on failure.
 */
int wdriver_i2c_read_reg(uint16_t address,
                     uint32_t scl_speed_hz,
                     uint8_t reg,
                     uint8_t *data,
                     uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* WDRIVER_I2C_H */
