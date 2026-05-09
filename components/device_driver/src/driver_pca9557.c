/**
 * @file bsp_pca9557.c
 * @brief PCA9557 8 位 I2C IO 扩展器驱动实现。
 */
#include "driver_pca9557.h"

#include "bsp_common.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * @brief PCA9557 日志标签。
 */
static const char *TAG = "bsp_pca9557";

/**
 * @brief PCA9557 输入端口寄存器。
 */
#define PCA9557_REG_INPUT       0x00u

/**
 * @brief PCA9557 输出端口寄存器。
 */
#define PCA9557_REG_OUTPUT      0x01u

/**
 * @brief PCA9557 极性反转寄存器。
 */
#define PCA9557_REG_POLARITY    0x02u

/**
 * @brief PCA9557 配置寄存器。
 */
#define PCA9557_REG_CONFIG      0x03u

/**
 * @brief PCA9557 驱动对象。
 */
struct pca9557_dev {
    pca9557_interface_t *itf;        /**< I2C 接口函数指针。 */
    pca9557_config_t config;         /**< 当前设备配置。 */
    uint8_t output_cache;            /**< 输出寄存器缓存。 */
    uint8_t direction_cache;         /**< 方向寄存器缓存。 */
    uint8_t polarity_cache;          /**< 极性反转寄存器缓存。 */
};

/**
 * @brief 检查 PCA9557 引脚编号是否有效。
 *
 * @param[in] pin 引脚编号。
 * @return 有效返回 true；无效返回 false。
 */
static bool pca9557_is_valid_pin(pca9557_pin_t pin)
{
    return pin >= PCA9557_PIN_0 && pin < PCA9557_PIN_MAX;
}

/**
 * @brief 读取 PCA9557 单字节寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] reg 寄存器地址。
 * @param[out] value 寄存器值输出地址。
 * @return 成功返回 0；失败返回负值。
 */
static int pca9557_read_reg(pca9557_handle_t dev, uint8_t reg, uint8_t *value)
{
    if (dev == NULL || value == NULL) {
        BSP_LOGE(TAG, "PCA9557 读寄存器参数无效, dev=%p, value=%p", dev, value);
        return -1;
    }

    int ret = dev->itf->read_reg(reg, value, sizeof(*value));
    if (ret == 0) {
        BSP_LOGI(TAG, "PCA9557 读寄存器成功, reg=0x%02X, value=0x%02X",
                 (unsigned int)reg, (unsigned int)*value);
    } else {
        BSP_LOGE(TAG, "PCA9557 读寄存器失败, reg=0x%02X, ret=%d",
                 (unsigned int)reg, ret);
    }

    return ret;
}

/**
 * @brief 写入 PCA9557 单字节寄存器。
 *
 * @param[in] dev PCA9557 句柄。
 * @param[in] reg 寄存器地址。
 * @param[in] value 寄存器值。
 * @return 成功返回 0；失败返回负值。
 */
static int pca9557_write_reg(pca9557_handle_t dev, uint8_t reg, uint8_t value)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 写寄存器参数无效, dev=NULL");
        return -1;
    }

    int ret = dev->itf->write_reg(reg, &value, sizeof(value));
    if (ret == 0) {
        BSP_LOGI(TAG, "PCA9557 写寄存器成功, reg=0x%02X, value=0x%02X",
                 (unsigned int)reg, (unsigned int)value);
    } else {
        BSP_LOGE(TAG, "PCA9557 写寄存器失败, reg=0x%02X, value=0x%02X, ret=%d",
                 (unsigned int)reg, (unsigned int)value, ret);
    }

    return ret;
}

pca9557_handle_t pca9557_init(const pca9557_config_t *config,
                              pca9557_interface_t *itf)
{
    if (itf == NULL ||
        itf->write_reg == NULL ||
        itf->read_reg == NULL) {
        BSP_LOGE(TAG, "PCA9557 初始化失败: I2C 接口为空");
        return NULL;
    }

    pca9557_config_t dev_config = {
        .output_init = 0x00,
        .polarity_init = 0x00,
        .direction_init = 0xFF,
    };

    if (config != NULL) {
        dev_config = *config;
    }

    pca9557_handle_t dev = (pca9557_handle_t)calloc(1, sizeof(struct pca9557_dev));
    if (dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 初始化失败: 内存分配失败");
        return NULL;
    }

    dev->config = dev_config;
    dev->itf = itf;
    dev->output_cache = dev_config.output_init;
    dev->direction_cache = dev_config.direction_init;
    dev->polarity_cache = dev_config.polarity_init;

    if (pca9557_write_reg(dev, PCA9557_REG_OUTPUT, dev->output_cache) != 0 ||
        pca9557_write_reg(dev, PCA9557_REG_POLARITY, dev->polarity_cache) != 0 ||
        pca9557_write_reg(dev, PCA9557_REG_CONFIG, dev->direction_cache) != 0) {
        free(dev);
        BSP_LOGE(TAG, "PCA9557 初始化失败: 初始寄存器写入失败");
        return NULL;
    }

    BSP_LOGI(TAG, "PCA9557 初始化成功, output=0x%02X, polarity=0x%02X, direction=0x%02X",
             (unsigned int)dev->output_cache,
             (unsigned int)dev->polarity_cache,
             (unsigned int)dev->direction_cache);
    return dev;
}

void pca9557_deinit(pca9557_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    free(dev);
    BSP_LOGI(TAG, "PCA9557 驱动已释放");
}

int pca9557_read_input(pca9557_handle_t dev, uint8_t *value)
{
    return pca9557_read_reg(dev, PCA9557_REG_INPUT, value);
}

int pca9557_read_output(pca9557_handle_t dev, uint8_t *value)
{
    if (dev == NULL || value == NULL) {
        BSP_LOGE(TAG, "PCA9557 读取输出缓存参数无效");
        return -1;
    }

    *value = dev->output_cache;
    BSP_LOGI(TAG, "PCA9557 输出缓存读取成功, value=0x%02X", (unsigned int)*value);
    return 0;
}

int pca9557_write_output(pca9557_handle_t dev, uint8_t value)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 写输出寄存器参数无效");
        return -1;
    }

    int ret = pca9557_write_reg(dev, PCA9557_REG_OUTPUT, value);
    if (ret == 0) {
        dev->output_cache = value;
        BSP_LOGI(TAG, "PCA9557 输出寄存器更新成功, value=0x%02X", (unsigned int)value);
    }

    return ret;
}

int pca9557_set_pin_mode(pca9557_handle_t dev,
                         pca9557_pin_t pin,
                         pca9557_io_mode_t mode)
{
    if (dev == NULL || !pca9557_is_valid_pin(pin)) {
        BSP_LOGE(TAG, "PCA9557 设置引脚方向参数无效, dev=%p, pin=%d", dev, pin);
        return -1;
    }

    uint8_t direction = dev->direction_cache;
    if (mode == PCA9557_IO_INPUT) {
        direction |= (uint8_t)(1u << pin);
    } else {
        direction &= (uint8_t)~(1u << pin);
    }

    return pca9557_set_direction(dev, direction);
}

int pca9557_set_direction(pca9557_handle_t dev, uint8_t direction)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 设置方向寄存器参数无效");
        return -1;
    }

    int ret = pca9557_write_reg(dev, PCA9557_REG_CONFIG, direction);
    if (ret == 0) {
        dev->direction_cache = direction;
        BSP_LOGI(TAG, "PCA9557 方向寄存器更新成功, direction=0x%02X",
                 (unsigned int)direction);
    }

    return ret;
}

int pca9557_set_pin_level(pca9557_handle_t dev,
                          pca9557_pin_t pin,
                          pca9557_level_t level)
{
    if (dev == NULL || !pca9557_is_valid_pin(pin)) {
        BSP_LOGE(TAG, "PCA9557 设置引脚电平参数无效, dev=%p, pin=%d", dev, pin);
        return -1;
    }

    uint8_t output = dev->output_cache;
    if (level == PCA9557_LEVEL_HIGH) {
        output |= (uint8_t)(1u << pin);
    } else {
        output &= (uint8_t)~(1u << pin);
    }

    return pca9557_write_output(dev, output);
}

int pca9557_get_pin_level(pca9557_handle_t dev,
                          pca9557_pin_t pin,
                          pca9557_level_t *level)
{
    if (!pca9557_is_valid_pin(pin) || level == NULL) {
        BSP_LOGE(TAG, "PCA9557 读取引脚电平参数无效, pin=%d, level=%p", pin, level);
        return -1;
    }

    uint8_t input = 0;
    int ret = pca9557_read_input(dev, &input);
    if (ret != 0) {
        return ret;
    }

    *level = (input & (uint8_t)(1u << pin)) ? PCA9557_LEVEL_HIGH : PCA9557_LEVEL_LOW;
    BSP_LOGI(TAG, "PCA9557 引脚电平读取成功, pin=%d, level=%d", pin, *level);
    return 0;
}

int pca9557_set_polarity(pca9557_handle_t dev, uint8_t polarity)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 设置极性寄存器参数无效");
        return -1;
    }

    int ret = pca9557_write_reg(dev, PCA9557_REG_POLARITY, polarity);
    if (ret == 0) {
        dev->polarity_cache = polarity;
        BSP_LOGI(TAG, "PCA9557 极性寄存器更新成功, polarity=0x%02X",
                 (unsigned int)polarity);
    }

    return ret;
}
