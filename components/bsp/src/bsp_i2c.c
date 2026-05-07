/**
 * @file bsp_i2c.c
 * @brief 基于 ESP-IDF 新版 I2C master 驱动的 BSP I2C 实现。
 */
#include "bsp_i2c.h"

#include "driver/i2c_master.h"
#include "hal/gpio_types.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief I2C 日志标签。
 */
static const char *TAG = "bsp_i2c";

/**
 * @brief 项目默认 I2C 总线句柄。
 */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/**
 * @brief PCA9557 的 I2C 设备句柄。
 */
static i2c_master_dev_handle_t s_pca9557_i2c_dev = NULL;

/**
 * @brief ES7210 的 I2C 设备句柄。
 */
static i2c_master_dev_handle_t s_es7210_i2c_dev = NULL;

/**
 * @brief 当前 ES7210 使用的 I2C 地址。
 */
static uint16_t s_es7210_i2c_addr = I2C_ADDR_ES7210;

/**
 * @brief ES8311 的 I2C 设备句柄。
 */
static i2c_master_dev_handle_t s_es8311_i2c_dev = NULL;

/**
 * @brief 将底层驱动错误码转换为 BSP 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int bsp_i2c_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/**
 * @brief 向 I2C 总线添加一个设备。
 *
 * @param[in] address 7-bit I2C 设备地址。
 * @param[in] scl_speed_hz 该设备使用的 SCL 频率。
 * @param[out] out_handle I2C 设备句柄输出地址。
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2c_add_device(uint16_t address,
                              uint32_t scl_speed_hz,
                              i2c_master_dev_handle_t *out_handle)
{
    if (out_handle == NULL || s_i2c_bus == NULL) {
        BSP_LOGE(TAG, "I2C 添加设备参数无效, out_handle=%p, bus=%p", out_handle, s_i2c_bus);
        return -1;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };

    int ret = bsp_i2c_err_to_int(i2c_master_bus_add_device(s_i2c_bus, &dev_config, out_handle));
    if (ret == 0) {
        BSP_LOGI(TAG, "I2C 设备添加成功, addr=0x%02X, speed=%u",
                 (unsigned int)address, (unsigned int)scl_speed_hz);
    } else {
        BSP_LOGE(TAG, "I2C 设备添加失败, addr=0x%02X, ret=%d",
                 (unsigned int)address, ret);
    }

    return ret;
}

/**
 * @brief 从 I2C 总线移除一个设备。
 *
 * @param[in] dev_handle I2C 设备句柄。
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2c_remove_device(i2c_master_dev_handle_t dev_handle)
{
    if (dev_handle == NULL) {
        BSP_LOGE(TAG, "I2C 移除设备参数无效");
        return -1;
    }

    int ret = bsp_i2c_err_to_int(i2c_master_bus_rm_device(dev_handle));
    if (ret == 0) {
        BSP_LOGI(TAG, "I2C 设备移除成功");
    } else {
        BSP_LOGE(TAG, "I2C 设备移除失败, ret=%d", ret);
    }

    return ret;
}

/**
 * @brief 写入 I2C 设备寄存器。
 *
 * @param[in] dev_handle I2C 设备句柄。
 * @param[in] reg 寄存器地址。
 * @param[in] data 待写入数据。
 * @param[in] len 待写入数据长度，单位为字节。
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2c_write_reg(i2c_master_dev_handle_t dev_handle,
                             uint8_t reg,
                             const uint8_t *data,
                             uint16_t len)
{
    if (dev_handle == NULL || (data == NULL && len > 0)) {
        BSP_LOGE(TAG, "I2C 写寄存器参数无效, dev=%p, data=%p, len=%u",
                 dev_handle, data, (unsigned int)len);
        return -1;
    }

    uint8_t *write_buf = (uint8_t *)calloc((size_t)len + 1u, sizeof(uint8_t));
    if (write_buf == NULL) {
        BSP_LOGE(TAG, "I2C 写寄存器失败: 内存分配失败, len=%u", (unsigned int)len);
        return -2;
    }

    write_buf[0] = reg;
    if (len > 0) {
        memcpy(&write_buf[1], data, len);
    }

    int ret = i2c_master_transmit(dev_handle,
                                  write_buf,
                                  (size_t)len + 1u,
                                  BSP_I2C_XFER_TIMEOUT_MS);
    free(write_buf);

    ret = bsp_i2c_err_to_int(ret);
    if (ret == 0) {
        BSP_LOGI(TAG, "I2C 写寄存器成功, reg=0x%02X, len=%u",
                 (unsigned int)reg, (unsigned int)len);
    } else {
        BSP_LOGE(TAG, "I2C 写寄存器失败, reg=0x%02X, len=%u, ret=%d",
                 (unsigned int)reg, (unsigned int)len, ret);
    }

    return ret;
}

/**
 * @brief 读取 I2C 设备寄存器。
 *
 * @param[in] dev_handle I2C 设备句柄。
 * @param[in] reg 寄存器地址。
 * @param[out] data 读取数据输出缓冲区。
 * @param[in] len 读取数据长度，单位为字节。
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2c_read_reg(i2c_master_dev_handle_t dev_handle,
                            uint8_t reg,
                            uint8_t *data,
                            uint16_t len)
{
    if (dev_handle == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "I2C 读寄存器参数无效, dev=%p, data=%p, len=%u",
                 dev_handle, data, (unsigned int)len);
        return -1;
    }

    int ret = bsp_i2c_err_to_int(i2c_master_transmit_receive(dev_handle,
                                                             &reg,
                                                             sizeof(reg),
                                                             data,
                                                             len,
                                                             BSP_I2C_XFER_TIMEOUT_MS));
    if (ret == 0) {
        BSP_LOGI(TAG, "I2C 读寄存器成功, reg=0x%02X, len=%u",
                 (unsigned int)reg, (unsigned int)len);
    } else {
        BSP_LOGE(TAG, "I2C 读寄存器失败, reg=0x%02X, len=%u, ret=%d",
                 (unsigned int)reg, (unsigned int)len, ret);
    }

    return ret;
}

int bsp_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        BSP_LOGI(TAG, "I2C 已初始化");
        return 0;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = (i2c_port_num_t)I2C_PORT_PCA9557,
        .sda_io_num = (gpio_num_t)BSP_I2C_SDA_IO,
        .scl_io_num = (gpio_num_t)BSP_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };

    int ret = bsp_i2c_err_to_int(i2c_new_master_bus(&bus_config, &s_i2c_bus));
    if (ret != 0) {
        BSP_LOGE(TAG, "I2C 总线初始化失败, ret=%d", ret);
        return ret;
    }
    BSP_LOGI(TAG, "I2C 总线初始化成功, sda=%d, scl=%d",
             BSP_I2C_SDA_IO, BSP_I2C_SCL_IO);

    ret = bsp_i2c_add_device(I2C_ADDR_PCA9557,
                             I2C_PCA9557_SCL_SPEED_HZ,
                             &s_pca9557_i2c_dev);
    if (ret != 0) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        BSP_LOGE(TAG, "PCA9557 I2C 设备初始化失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_i2c_add_device(I2C_ADDR_ES7210,
                             I2C_AUDIO_CODEC_SCL_SPEED_HZ,
                             &s_es7210_i2c_dev);
    if (ret != 0) {
        bsp_i2c_remove_device(s_pca9557_i2c_dev);
        s_pca9557_i2c_dev = NULL;
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        BSP_LOGE(TAG, "ES7210 I2C 设备初始化失败, ret=%d", ret);
        return ret;
    }
    s_es7210_i2c_addr = I2C_ADDR_ES7210;

    ret = bsp_i2c_add_device(I2C_ADDR_ES8311,
                             I2C_AUDIO_CODEC_SCL_SPEED_HZ,
                             &s_es8311_i2c_dev);
    if (ret != 0) {
        bsp_i2c_remove_device(s_es7210_i2c_dev);
        s_es7210_i2c_dev = NULL;
        bsp_i2c_remove_device(s_pca9557_i2c_dev);
        s_pca9557_i2c_dev = NULL;
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        BSP_LOGE(TAG, "ES8311 I2C 设备初始化失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "BSP I2C 初始化完成");
    return 0;
}

int bsp_i2c_deinit(void)
{
    if (s_i2c_bus == NULL) {
        BSP_LOGI(TAG, "I2C 未初始化，无需释放");
        return 0;
    }

    if (s_pca9557_i2c_dev != NULL) {
        bsp_i2c_remove_device(s_pca9557_i2c_dev);
        s_pca9557_i2c_dev = NULL;
    }

    if (s_es7210_i2c_dev != NULL) {
        bsp_i2c_remove_device(s_es7210_i2c_dev);
        s_es7210_i2c_dev = NULL;
    }
    s_es7210_i2c_addr = I2C_ADDR_ES7210;

    if (s_es8311_i2c_dev != NULL) {
        bsp_i2c_remove_device(s_es8311_i2c_dev);
        s_es8311_i2c_dev = NULL;
    }

    int ret = i2c_del_master_bus(s_i2c_bus);
    if (ret == 0) {
        s_i2c_bus = NULL;
        BSP_LOGI(TAG, "I2C 总线释放成功");
    } else {
        BSP_LOGE(TAG, "I2C 总线释放失败, ret=%d", ret);
    }

    return bsp_i2c_err_to_int(ret);
}

void *bsp_i2c_get_bus_handle(void)
{
    return (void *)s_i2c_bus;
}

int pca9557_i2c_write_reg_impl(uint8_t reg,
                               const uint8_t *data,
                               uint16_t len)
{
    if (s_pca9557_i2c_dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 I2C 写失败: 设备未初始化");
        return -1;
    }

    return bsp_i2c_write_reg(s_pca9557_i2c_dev, reg, data, len);
}

int pca9557_i2c_read_reg_impl(uint8_t reg,
                              uint8_t *data,
                              uint16_t len)
{
    if (s_pca9557_i2c_dev == NULL) {
        BSP_LOGE(TAG, "PCA9557 I2C 读失败: 设备未初始化");
        return -1;
    }

    return bsp_i2c_read_reg(s_pca9557_i2c_dev, reg, data, len);
}

int es7210_i2c_write_reg_impl(uint8_t reg,
                              const uint8_t *data,
                              uint16_t len)
{
    if (s_es7210_i2c_dev == NULL) {
        BSP_LOGE(TAG, "ES7210 I2C 写失败: 设备未初始化");
        return -1;
    }

    int ret = bsp_i2c_write_reg(s_es7210_i2c_dev, reg, data, len);
    if (ret == 0 || reg != 0x00u || s_es7210_i2c_addr == I2C_ADDR_ES7210_ALT) {
        return ret;
    }

    BSP_LOGW(TAG,
             "ES7210 addr=0x%02X 首次写入 NACK，尝试兼容地址 0x%02X",
             (unsigned int)s_es7210_i2c_addr,
             (unsigned int)I2C_ADDR_ES7210_ALT);

    (void)bsp_i2c_remove_device(s_es7210_i2c_dev);
    s_es7210_i2c_dev = NULL;

    int add_ret = bsp_i2c_add_device(I2C_ADDR_ES7210_ALT,
                                     I2C_AUDIO_CODEC_SCL_SPEED_HZ,
                                     &s_es7210_i2c_dev);
    if (add_ret != 0) {
        s_es7210_i2c_addr = I2C_ADDR_ES7210;
        (void)bsp_i2c_add_device(I2C_ADDR_ES7210,
                                 I2C_AUDIO_CODEC_SCL_SPEED_HZ,
                                 &s_es7210_i2c_dev);
        return ret;
    }

    s_es7210_i2c_addr = I2C_ADDR_ES7210_ALT;
    ret = bsp_i2c_write_reg(s_es7210_i2c_dev, reg, data, len);
    if (ret == 0) {
        BSP_LOGI(TAG, "ES7210 使用兼容地址 0x%02X 通信成功",
                 (unsigned int)s_es7210_i2c_addr);
    }

    return ret;
}

int es7210_i2c_read_reg_impl(uint8_t reg,
                             uint8_t *data,
                             uint16_t len)
{
    if (s_es7210_i2c_dev == NULL) {
        BSP_LOGE(TAG, "ES7210 I2C 读失败: 设备未初始化");
        return -1;
    }

    return bsp_i2c_read_reg(s_es7210_i2c_dev, reg, data, len);
}

int es8311_i2c_write_reg_impl(uint8_t reg,
                              const uint8_t *data,
                              uint16_t len)
{
    if (s_es8311_i2c_dev == NULL) {
        BSP_LOGE(TAG, "ES8311 I2C 写失败: 设备未初始化");
        return -1;
    }

    return bsp_i2c_write_reg(s_es8311_i2c_dev, reg, data, len);
}

int es8311_i2c_read_reg_impl(uint8_t reg,
                             uint8_t *data,
                             uint16_t len)
{
    if (s_es8311_i2c_dev == NULL) {
        BSP_LOGE(TAG, "ES8311 I2C 读失败: 设备未初始化");
        return -1;
    }

    return bsp_i2c_read_reg(s_es8311_i2c_dev, reg, data, len);
}
