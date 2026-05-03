/**
 * @file bsp_pca9557.h
 * @brief PCA9557 8 位 I2C IO 扩展器驱动接口。
 *
 * PCA9557 提供输入、输出、极性反转和方向配置四个 8 位寄存器。本驱动通过
 * @ref pca9557_interface_t 注入 I2C 访问接口，不直接依赖具体芯片 SDK。
 */
#ifndef BSP_PCA9557_H
#define BSP_PCA9557_H

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
 * @brief PCA9557 驱动依赖的 I2C 接口抽象。
 *
 * 用户需提供 PCA9557 寄存器读写函数，驱动不感知底层 I2C 端口、地址和设备句柄。
 */
typedef struct {
    int (*write_reg)(uint8_t reg,
                     const uint8_t *data,
                     uint16_t len);                   /**< 写 I2C 设备寄存器函数。 */
    int (*read_reg)(uint8_t reg,
                    uint8_t *data,
                    uint16_t len);                    /**< 读 I2C 设备寄存器函数。 */
} pca9557_interface_t;

/**
 * @brief PCA9557 驱动配置参数。
 */
typedef struct {
    uint8_t output_init;      /**< 输出寄存器初始值。 */
    uint8_t polarity_init;    /**< 极性反转寄存器初始值，1 表示对应输入取反。 */
    uint8_t direction_init;   /**< 方向寄存器初始值，1 输入，0 输出。 */
} pca9557_config_t;

/**
 * @brief PCA9557 模块句柄。
 */
typedef struct pca9557_dev *pca9557_handle_t;

/**
 * @brief 初始化 PCA9557 设备。
 *
 * @param[in] config 设备配置；传入 NULL 时使用默认配置。
 * @param[in] itf I2C 接口函数指针集合。
 * @return 成功返回 PCA9557 句柄；失败返回 NULL。
 * @note 调用本函数前应先完成底层 I2C 总线初始化。
 */
pca9557_handle_t pca9557_init(const pca9557_config_t *config,
                              pca9557_interface_t *itf);

/**
 * @brief 释放 PCA9557 设备。
 *
 * @param[in] dev PCA9557 句柄。
 */
void pca9557_deinit(pca9557_handle_t dev);

/**
 * @brief 读取 PCA9557 输入寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[out] value 输入寄存器值输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_read_input(pca9557_handle_t dev, uint8_t *value);

/**
 * @brief 读取 PCA9557 输出寄存器缓存值。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[out] value 输出寄存器值输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_read_output(pca9557_handle_t dev, uint8_t *value);

/**
 * @brief 写入 PCA9557 输出寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] value 输出寄存器值。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_write_output(pca9557_handle_t dev, uint8_t value);

/**
 * @brief 设置单个 PCA9557 引脚方向。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] pin 引脚编号。
 * @param[in] mode 引脚方向。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_set_pin_mode(pca9557_handle_t dev,
                         pca9557_pin_t pin,
                         pca9557_io_mode_t mode);

/**
 * @brief 设置 PCA9557 方向寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] direction 方向寄存器值，1 输入，0 输出。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_set_direction(pca9557_handle_t dev, uint8_t direction);

/**
 * @brief 设置单个 PCA9557 输出引脚电平。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] pin 引脚编号。
 * @param[in] level 输出电平。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_set_pin_level(pca9557_handle_t dev,
                          pca9557_pin_t pin,
                          pca9557_level_t level);

/**
 * @brief 读取单个 PCA9557 输入引脚电平。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] pin 引脚编号。
 * @param[out] level 引脚电平输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_get_pin_level(pca9557_handle_t dev,
                          pca9557_pin_t pin,
                          pca9557_level_t *level);

/**
 * @brief 设置 PCA9557 极性反转寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] polarity 极性反转寄存器值，1 表示对应输入取反。
 * @return 成功返回 0；失败返回负值。
 */
int pca9557_set_polarity(pca9557_handle_t dev, uint8_t polarity);

#ifdef __cplusplus
}
#endif

#endif /* BSP_PCA9557_H */
