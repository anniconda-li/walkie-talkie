/**
 * @file d_pca9557.c
 * @brief PCA9557 8 位 I2C IO 扩展器驱动实现。
 */
#include "d_pca9557.h"

#include "d_config.h"

#include <stdbool.h>
#include <string.h>

/**
 * @brief PCA9557 日志标签。
 */
static const char *TAG = "d_pca9557";

#define D_PCA9557_I2C_ADDR        0x19u
#define D_PCA9557_I2C_SPEED_HZ    100000u
#define D_PCA9557_LCD_BL_PIN      PCA9557_PIN_5
#define D_PCA9557_CAMERA_PWDN_PIN PCA9557_PIN_1

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

typedef struct {
    int (*write_reg)(uint8_t reg, const uint8_t *data, uint16_t len);
    int (*read_reg)(uint8_t reg, uint8_t *data, uint16_t len);
} pca9557_interface_t;

typedef struct {
    uint8_t output_init;
    uint8_t polarity_init;
    uint8_t direction_init;
} pca9557_config_t;

/**
 * @brief PCA9557 驱动对象。
 */
typedef struct {
    pca9557_interface_t itf;         /**< I2C 接口函数指针。 */
    pca9557_config_t config;         /**< 当前设备配置。 */
    uint8_t output_cache;            /**< 输出寄存器缓存。 */
    uint8_t direction_cache;         /**< 方向寄存器缓存。 */
    uint8_t polarity_cache;          /**< 极性反转寄存器缓存。 */
} pca9557_dev_t;

static pca9557_dev_t s_board_pca9557;
static uint8_t s_board_pca9557_inited = 0u;
static d_pca9557_wdriver_ops_t s_board_ops;

static int d_pca9557_write_reg(uint8_t reg, const uint8_t *data, uint16_t len)
{
    if (s_board_ops.i2c_write_reg == NULL) {
        return -1;
    }

    return s_board_ops.i2c_write_reg(D_PCA9557_I2C_ADDR,
                                     D_PCA9557_I2C_SPEED_HZ,
                                     reg,
                                     data,
                                     len);
}

static int d_pca9557_read_reg(uint8_t reg, uint8_t *data, uint16_t len)
{
    if (s_board_ops.i2c_read_reg == NULL) {
        return -1;
    }

    return s_board_ops.i2c_read_reg(D_PCA9557_I2C_ADDR,
                                    D_PCA9557_I2C_SPEED_HZ,
                                    reg,
                                    data,
                                    len);
}

static pca9557_interface_t s_board_pca9557_itf = {
    .write_reg = d_pca9557_write_reg,
    .read_reg = d_pca9557_read_reg,
};

static int pca9557_set_direction(uint8_t direction);

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
static int pca9557_read_reg(uint8_t reg, uint8_t *value)
{
    if (s_board_pca9557_inited == 0u || value == NULL) {
        D_LOGE(TAG, "PCA9557 读寄存器参数无效, inited=%u, value=%p",
                    (unsigned int)s_board_pca9557_inited, value);
        return -1;
    }

    int ret = s_board_pca9557.itf.read_reg(reg, value, sizeof(*value));
    if (ret == 0) {
        D_LOGI(TAG, "PCA9557 读寄存器成功, reg=0x%02X, value=0x%02X",
                 (unsigned int)reg, (unsigned int)*value);
    } else {
        D_LOGE(TAG, "PCA9557 读寄存器失败, reg=0x%02X, ret=%d",
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
static int pca9557_write_reg(uint8_t reg, uint8_t value)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 写寄存器参数无效: 未初始化");
        return -1;
    }

    int ret = s_board_pca9557.itf.write_reg(reg, &value, sizeof(value));
    if (ret == 0) {
        D_LOGI(TAG, "PCA9557 写寄存器成功, reg=0x%02X, value=0x%02X",
                 (unsigned int)reg, (unsigned int)value);
    } else {
        D_LOGE(TAG, "PCA9557 写寄存器失败, reg=0x%02X, value=0x%02X, ret=%d",
                 (unsigned int)reg, (unsigned int)value, ret);
    }

    return ret;
}

static int pca9557_init(const pca9557_config_t *config, pca9557_interface_t *itf)
{
    if (itf == NULL ||
        itf->write_reg == NULL ||
        itf->read_reg == NULL) {
        D_LOGE(TAG, "PCA9557 初始化失败: I2C 接口为空");
        return -1;
    }

    pca9557_config_t dev_config = {
        .output_init = 0x00,
        .polarity_init = 0x00,
        .direction_init = 0xFF,
    };

    if (config != NULL) {
        dev_config = *config;
    }

    memset(&s_board_pca9557, 0, sizeof(s_board_pca9557));
    s_board_pca9557.config = dev_config;
    s_board_pca9557.itf = *itf;
    s_board_pca9557.output_cache = dev_config.output_init;
    s_board_pca9557.direction_cache = dev_config.direction_init;
    s_board_pca9557.polarity_cache = dev_config.polarity_init;
    s_board_pca9557_inited = 1u;

    if (pca9557_write_reg(PCA9557_REG_OUTPUT, s_board_pca9557.output_cache) != 0 ||
        pca9557_write_reg(PCA9557_REG_POLARITY, s_board_pca9557.polarity_cache) != 0 ||
        pca9557_write_reg(PCA9557_REG_CONFIG, s_board_pca9557.direction_cache) != 0) {
        memset(&s_board_pca9557, 0, sizeof(s_board_pca9557));
        s_board_pca9557_inited = 0u;
        D_LOGE(TAG, "PCA9557 初始化失败: 初始寄存器写入失败");
        return -2;
    }

    D_LOGI(TAG, "PCA9557 初始化成功, output=0x%02X, polarity=0x%02X, direction=0x%02X",
             (unsigned int)s_board_pca9557.output_cache,
             (unsigned int)s_board_pca9557.polarity_cache,
             (unsigned int)s_board_pca9557.direction_cache);
    return 0;
}

static void pca9557_deinit(void)
{
    if (s_board_pca9557_inited == 0u) {
        return;
    }

    memset(&s_board_pca9557, 0, sizeof(s_board_pca9557));
    s_board_pca9557_inited = 0u;
    D_LOGI(TAG, "PCA9557 驱动已释放");
}

static int pca9557_read_input(uint8_t *value)
{
    return pca9557_read_reg(PCA9557_REG_INPUT, value);
}

static int pca9557_write_output(uint8_t value)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 写输出寄存器失败: 未初始化");
        return -1;
    }

    int ret = pca9557_write_reg(PCA9557_REG_OUTPUT, value);
    if (ret == 0) {
        s_board_pca9557.output_cache = value;
        D_LOGI(TAG, "PCA9557 输出寄存器更新成功, value=0x%02X", (unsigned int)value);
    }

    return ret;
}

static int pca9557_set_pin_mode(pca9557_pin_t pin,
                                pca9557_io_mode_t mode)
{
    if (s_board_pca9557_inited == 0u || !pca9557_is_valid_pin(pin)) {
        D_LOGE(TAG, "PCA9557 设置引脚方向参数无效, inited=%u, pin=%d",
                    (unsigned int)s_board_pca9557_inited, pin);
        return -1;
    }

    uint8_t direction = s_board_pca9557.direction_cache;
    if (mode == PCA9557_IO_INPUT) {
        direction |= (uint8_t)(1u << pin);
    } else {
        direction &= (uint8_t)~(1u << pin);
    }

    return pca9557_set_direction(direction);
}

static int pca9557_set_direction(uint8_t direction)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 设置方向寄存器失败: 未初始化");
        return -1;
    }

    int ret = pca9557_write_reg(PCA9557_REG_CONFIG, direction);
    if (ret == 0) {
        s_board_pca9557.direction_cache = direction;
        D_LOGI(TAG, "PCA9557 方向寄存器更新成功, direction=0x%02X",
                 (unsigned int)direction);
    }

    return ret;
}

static int pca9557_set_pin_level(pca9557_pin_t pin,
                                 pca9557_level_t level)
{
    if (s_board_pca9557_inited == 0u || !pca9557_is_valid_pin(pin)) {
        D_LOGE(TAG, "PCA9557 设置引脚电平参数无效, inited=%u, pin=%d",
                    (unsigned int)s_board_pca9557_inited, pin);
        return -1;
    }

    uint8_t output = s_board_pca9557.output_cache;
    if (level == PCA9557_LEVEL_HIGH) {
        output |= (uint8_t)(1u << pin);
    } else {
        output &= (uint8_t)~(1u << pin);
    }

    return pca9557_write_output(output);
}

static int pca9557_get_pin_level(pca9557_pin_t pin,
                                 pca9557_level_t *level)
{
    if (s_board_pca9557_inited == 0u || !pca9557_is_valid_pin(pin) || level == NULL) {
        D_LOGE(TAG, "PCA9557 读取引脚电平参数无效, inited=%u, pin=%d, level=%p",
                    (unsigned int)s_board_pca9557_inited, pin, level);
        return -1;
    }

    uint8_t input = 0;
    int ret = pca9557_read_input(&input);
    if (ret != 0) {
        return ret;
    }

    *level = (input & (uint8_t)(1u << pin)) ? PCA9557_LEVEL_HIGH : PCA9557_LEVEL_LOW;
    D_LOGI(TAG, "PCA9557 引脚电平读取成功, pin=%d, level=%d", pin, *level);
    return 0;
}

int d_pca9557_init(const d_pca9557_wdriver_ops_t *ops)
{
    if (s_board_pca9557_inited != 0u) {
        D_LOGI(TAG, "本板 PCA9557 已初始化");
        return 0;
    }

    if (ops == NULL ||
        ops->get_i2c_bus_handle == NULL ||
        ops->i2c_write_reg == NULL ||
        ops->i2c_read_reg == NULL) {
        D_LOGE(TAG, "本板 PCA9557 初始化失败: WDRIVER I2C 能力为空");
        return -1;
    }

    if (ops->get_i2c_bus_handle() == NULL) {
        D_LOGE(TAG, "本板 PCA9557 初始化失败: I2C 未初始化");
        return -2;
    }

    s_board_ops = *ops;

    pca9557_config_t config = {
        .output_init = 0x00u,
        .polarity_init = 0x00u,
        .direction_init = (uint8_t)~((1u << D_PCA9557_CAMERA_PWDN_PIN) |
                                     (1u << D_PCA9557_LCD_BL_PIN)),
    };

    if (pca9557_init(&config, &s_board_pca9557_itf) != 0) {
        D_LOGE(TAG, "本板 PCA9557 初始化失败");
        return -3;
    }

    D_LOGI(TAG, "本板 PCA9557 初始化成功");
    return 0;
}

int d_pca9557_deinit(void)
{
    if (s_board_pca9557_inited == 0u) {
        return 0;
    }

    pca9557_deinit();
    s_board_ops = (d_pca9557_wdriver_ops_t){0};
    D_LOGI(TAG, "本板 PCA9557 已释放");
    return 0;
}

int d_pca9557_is_initialized(void)
{
    return (s_board_pca9557_inited != 0u) ? 1 : 0;
}

int d_pca9557_set_pin_mode(pca9557_pin_t pin, pca9557_io_mode_t mode)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 设置引脚方向失败: 未初始化");
        return -1;
    }

    return pca9557_set_pin_mode(pin, mode);
}

int d_pca9557_set_pin_level(pca9557_pin_t pin, pca9557_level_t level)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 设置引脚电平失败: 未初始化");
        return -1;
    }

    return pca9557_set_pin_level(pin, level);
}

int d_pca9557_get_pin_level(pca9557_pin_t pin, pca9557_level_t *level)
{
    if (s_board_pca9557_inited == 0u) {
        D_LOGE(TAG, "PCA9557 读取引脚电平失败: 未初始化");
        return -1;
    }

    return pca9557_get_pin_level(pin, level);
}

int d_pca9557_set_lcd_backlight(int on)
{
    int ret = d_pca9557_set_pin_level(D_PCA9557_LCD_BL_PIN,
                                           on != 0 ? PCA9557_LEVEL_HIGH : PCA9557_LEVEL_LOW);
    if (ret == 0) {
        D_LOGI(TAG, "LCD 背光%s", on != 0 ? "打开" : "关闭");
    } else {
        D_LOGE(TAG, "LCD 背光控制失败, ret=%d", ret);
    }

    return ret;
}

int d_pca9557_set_camera_pwdn(pca9557_level_t level)
{
    int ret = d_pca9557_set_pin_level(D_PCA9557_CAMERA_PWDN_PIN, level);
    if (ret == 0) {
        D_LOGI(TAG, "Camera PWDN 设置成功, level=%d", level);
    } else {
        D_LOGE(TAG, "Camera PWDN 设置失败, ret=%d", ret);
    }

    return ret;
}
