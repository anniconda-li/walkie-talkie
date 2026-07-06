/**
 * @file d_pca9557.h
 * @brief PCA9557 8 位 I2C IO 扩展器驱动接口。
 *
 * PCA9557 提供输入、输出、极性反转和方向配置四个 8 位寄存器。本驱动作为
 * 当前板级共享 IO 管理者，对外提供 pin 级和板级语义接口。
 */
#ifndef D_PCA9557_H
#define D_PCA9557_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCA9557 引脚编号。
 */
typedef enum {
    PCA9557_PIN_0 = 0, /**< P0。 */
    PCA9557_PIN_1,     /**< P1。 */
    PCA9557_PIN_2,     /**< P2。 */
    PCA9557_PIN_3,     /**< P3。 */
    PCA9557_PIN_4,     /**< P4。 */
    PCA9557_PIN_5,     /**< P5。 */
    PCA9557_PIN_6,     /**< P6。 */
    PCA9557_PIN_7,     /**< P7。 */
    PCA9557_PIN_MAX,   /**< 引脚数量。 */
} pca9557_pin_t;

/**
 * @brief PCA9557 引脚方向。
 */
typedef enum {
    PCA9557_IO_OUTPUT = 0, /**< 输出模式。 */
    PCA9557_IO_INPUT = 1,  /**< 输入模式。 */
} pca9557_io_mode_t;

/**
 * @brief PCA9557 引脚电平。
 */
typedef enum {
    PCA9557_LEVEL_LOW = 0,  /**< 低电平。 */
    PCA9557_LEVEL_HIGH = 1, /**< 高电平。 */
} pca9557_level_t;

/**
 * @brief 本板 PCA9557 驱动依赖的 WDRIVER I2C 能力。
 */
typedef struct {
    void *(*get_i2c_bus_handle)(void);                 /**< 获取 I2C bus 句柄，用于确认 WDRIVER I2C 已初始化。 */
    int (*i2c_write_reg)(uint16_t address,
                         uint32_t scl_speed_hz,
                         uint8_t reg,
                         const uint8_t *data,
                         uint16_t len);                /**< 写 I2C 设备寄存器。 */
    int (*i2c_read_reg)(uint16_t address,
                        uint32_t scl_speed_hz,
                        uint8_t reg,
                        uint8_t *data,
                        uint16_t len);                 /**< 读 I2C 设备寄存器。 */
} d_pca9557_wdriver_ops_t;

/**
 * @brief 设置单个 PCA9557 引脚方向。
 *
 * @param[in] pin 引脚编号。
 * @param[in] mode 引脚方向。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_set_pin_mode(pca9557_pin_t pin, pca9557_io_mode_t mode);

/**
 * @brief 设置单个 PCA9557 引脚输出电平。
 *
 * @param[in] pin 引脚编号。
 * @param[in] level 输出电平。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_set_pin_level(pca9557_pin_t pin, pca9557_level_t level);

/**
 * @brief 读取单个 PCA9557 输入引脚电平。
 *
 * @param[in] pin 引脚编号。
 * @param[out] level 引脚电平输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_get_pin_level(pca9557_pin_t pin, pca9557_level_t *level);

/**
 * @brief 初始化本板 PCA9557 IO 扩展器。
 *
 * @param[in] ops WDRIVER I2C 能力。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_init(const d_pca9557_wdriver_ops_t *ops);

/**
 * @brief 释放本板 PCA9557 IO 扩展器。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_deinit(void);

/**
 * @brief 判断本板 PCA9557 是否已初始化。
 *
 * @return 已初始化返回 1；否则返回 0。
 */
int d_pca9557_is_initialized(void);

/**
 * @brief 设置电池采样使能。
 *
 * PCA9557 IO4 低电平使能电池分压采样，高电平关闭采样。
 *
 * @param[in] enabled 0 关闭采样，非 0 使能采样。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_set_battery_measurement_enabled(int enabled);

/**
 * @brief 设置 Camera PWDN 引脚电平。
 *
 * @param[in] level PCA9557 输出电平。
 * @return 成功返回 0；失败返回负值。
 */
int d_pca9557_set_camera_pwdn(pca9557_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* D_PCA9557_H */
