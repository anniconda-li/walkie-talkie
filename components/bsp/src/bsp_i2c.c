/**
 * @file bsp_i2c.c
 * @brief BSP I2C bus adapter implementation.
 */
#include "bsp_i2c.h"

#include "driver/i2c_master.h"
#include "hal/gpio_types.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_i2c_bus = NULL;

static int bsp_i2c_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

static int bsp_i2c_add_temp_device(uint16_t address,
                                   uint32_t scl_speed_hz,
                                   i2c_master_dev_handle_t *out_handle)
{
    if (s_i2c_bus == NULL || out_handle == NULL) {
        return -1;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };

    return bsp_i2c_err_to_int(i2c_master_bus_add_device(s_i2c_bus, &dev_config, out_handle));
}

int bsp_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        BSP_LOGI(TAG, "I2C 已初始化");
        return 0;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = (i2c_port_num_t)BSP_I2C_PORT,
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

    BSP_LOGI(TAG, "I2C 总线初始化成功, sda=%d, scl=%d", BSP_I2C_SDA_IO, BSP_I2C_SCL_IO);
    return 0;
}

int bsp_i2c_deinit(void)
{
    if (s_i2c_bus == NULL) {
        return 0;
    }

    int ret = bsp_i2c_err_to_int(i2c_del_master_bus(s_i2c_bus));
    if (ret == 0) {
        s_i2c_bus = NULL;
        BSP_LOGI(TAG, "I2C 总线释放成功");
    } else {
        BSP_LOGE(TAG, "I2C 总线释放失败, ret=%d", ret);
    }

    return ret;
}

void *bsp_i2c_get_bus_handle(void)
{
    return (void *)s_i2c_bus;
}

int bsp_i2c_write_reg(uint16_t address,
                      uint32_t scl_speed_hz,
                      uint8_t reg,
                      const uint8_t *data,
                      uint16_t len)
{
    if (s_i2c_bus == NULL || (data == NULL && len > 0u)) {
        return -1;
    }

    i2c_master_dev_handle_t dev = NULL;
    int ret = bsp_i2c_add_temp_device(address, scl_speed_hz, &dev);
    if (ret != 0) {
        return ret;
    }

    uint8_t *write_buf = (uint8_t *)calloc((size_t)len + 1u, sizeof(uint8_t));
    if (write_buf == NULL) {
        (void)i2c_master_bus_rm_device(dev);
        return -2;
    }

    write_buf[0] = reg;
    if (len > 0u) {
        memcpy(&write_buf[1], data, len);
    }

    ret = bsp_i2c_err_to_int(i2c_master_transmit(dev,
                                                 write_buf,
                                                 (size_t)len + 1u,
                                                 BSP_I2C_XFER_TIMEOUT_MS));
    free(write_buf);
    (void)i2c_master_bus_rm_device(dev);
    return ret;
}

int bsp_i2c_read_reg(uint16_t address,
                     uint32_t scl_speed_hz,
                     uint8_t reg,
                     uint8_t *data,
                     uint16_t len)
{
    if (s_i2c_bus == NULL || data == NULL || len == 0u) {
        return -1;
    }

    i2c_master_dev_handle_t dev = NULL;
    int ret = bsp_i2c_add_temp_device(address, scl_speed_hz, &dev);
    if (ret != 0) {
        return ret;
    }

    ret = bsp_i2c_err_to_int(i2c_master_transmit_receive(dev,
                                                         &reg,
                                                         sizeof(reg),
                                                         data,
                                                         len,
                                                         BSP_I2C_XFER_TIMEOUT_MS));
    (void)i2c_master_bus_rm_device(dev);
    return ret;
}
