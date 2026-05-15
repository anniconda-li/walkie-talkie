/**
 * @file bsp_i2c.c
 * @brief BSP I2C bus adapter implementation.
 */
#include "bsp_i2c.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "hal/gpio_types.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bsp_i2c";

/** @brief legacy I2C driver 是否已经安装。 */
static uint8_t s_i2c_inited = 0u;

static int bsp_i2c_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

int bsp_i2c_init(void)
{
    if (s_i2c_inited != 0u) {
        BSP_LOGI(TAG, "I2C 已初始化");
        return 0;
    }

    /*
     * ESP-IDF 5.3.x 的 esp32-camera SCCB 仍基于 legacy I2C driver。
     * 为了让 camera、PCA9557、FT5x06 共享同一条 I2C 总线，这里统一使用
     * legacy driver/i2c.h，避免和 new driver/i2c_master.h 混用触发 abort。
     */
    i2c_config_t bus_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA_IO,
        .scl_io_num = BSP_I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_SCL_SPEED_HZ,
        .clk_flags = 0,
    };

    int ret = bsp_i2c_err_to_int(i2c_param_config((i2c_port_t)BSP_I2C_PORT, &bus_config));
    if (ret == 0) {
        ret = bsp_i2c_err_to_int(i2c_driver_install((i2c_port_t)BSP_I2C_PORT,
                                                    I2C_MODE_MASTER,
                                                    0,
                                                    0,
                                                    0));
    }
    if (ret != 0) {
        BSP_LOGE(TAG, "I2C 总线初始化失败, ret=%d", ret);
        return ret;
    }

    s_i2c_inited = 1u;
    BSP_LOGI(TAG, "I2C 总线初始化成功, sda=%d, scl=%d", BSP_I2C_SDA_IO, BSP_I2C_SCL_IO);
    return 0;
}

int bsp_i2c_deinit(void)
{
    if (s_i2c_inited == 0u) {
        return 0;
    }

    int ret = bsp_i2c_err_to_int(i2c_driver_delete((i2c_port_t)BSP_I2C_PORT));
    if (ret == 0) {
        s_i2c_inited = 0u;
        BSP_LOGI(TAG, "I2C 总线释放成功");
    } else {
        BSP_LOGE(TAG, "I2C 总线释放失败, ret=%d", ret);
    }

    return ret;
}

void *bsp_i2c_get_bus_handle(void)
{
    return (s_i2c_inited != 0u) ? (void *)1 : NULL;
}

int bsp_i2c_write_reg(uint16_t address,
                      uint32_t scl_speed_hz,
                      uint8_t reg,
                      const uint8_t *data,
                      uint16_t len)
{
    if (s_i2c_inited == 0u || (data == NULL && len > 0u)) {
        return -1;
    }

    uint8_t *write_buf = (uint8_t *)calloc((size_t)len + 1u, sizeof(uint8_t));
    if (write_buf == NULL) {
        return -2;
    }

    write_buf[0] = reg;
    if (len > 0u) {
        memcpy(&write_buf[1], data, len);
    }

    (void)scl_speed_hz;
    int ret = bsp_i2c_err_to_int(i2c_master_write_to_device((i2c_port_t)BSP_I2C_PORT,
                                                            (uint8_t)address,
                                                            write_buf,
                                                            (size_t)len + 1u,
                                                            pdMS_TO_TICKS(BSP_I2C_XFER_TIMEOUT_MS)));
    free(write_buf);
    return ret;
}

int bsp_i2c_read_reg(uint16_t address,
                     uint32_t scl_speed_hz,
                     uint8_t reg,
                     uint8_t *data,
                     uint16_t len)
{
    if (s_i2c_inited == 0u || data == NULL || len == 0u) {
        return -1;
    }

    (void)scl_speed_hz;
    return bsp_i2c_err_to_int(i2c_master_write_read_device((i2c_port_t)BSP_I2C_PORT,
                                                           (uint8_t)address,
                                                           &reg,
                                                           sizeof(reg),
                                                           data,
                                                           len,
                                                           pdMS_TO_TICKS(BSP_I2C_XFER_TIMEOUT_MS)));
}
