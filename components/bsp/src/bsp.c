/**
 * @file bsp.c
 * @brief BSP 板级基础资源统一初始化实现。
 */
#include "bsp.h"

#include "bsp_config.h"
#include "bsp_i2c.h"
#include "bsp_i2s.h"
#include "bsp_spi.h"
#include "bsp_uart.h"

/**
 * @brief BSP 日志标签。
 */
static const char *TAG = "bsp";

int bsp_init(void)
{
    int ret = bsp_i2c_init();
    if (ret != 0) {
        BSP_LOGE(TAG, "BSP 初始化失败: I2C 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_spi_init();
    if (ret != 0) {
        BSP_LOGE(TAG, "BSP 初始化失败: SPI 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_uart_init();
    if (ret != 0) {
        BSP_LOGE(TAG, "BSP 初始化失败: UART 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_i2s_init();
    if (ret != 0) {
        BSP_LOGE(TAG, "BSP 初始化失败: I2S 初始化失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "BSP 基础资源初始化完成");
    return 0;
}

int bsp_deinit(void)
{
    int ret = 0;

    int i2s_ret = bsp_i2s_deinit();
    if (ret == 0 && i2s_ret != 0) {
        ret = i2s_ret;
    }

    int spi_ret = bsp_spi_deinit();
    if (ret == 0 && spi_ret != 0) {
        ret = spi_ret;
    }

    int i2c_ret = bsp_i2c_deinit();
    if (ret == 0 && i2c_ret != 0) {
        ret = i2c_ret;
    }

    if (ret == 0) {
        BSP_LOGI(TAG, "BSP 基础资源释放完成");
    } else {
        BSP_LOGE(TAG, "BSP 基础资源释放存在失败, ret=%d", ret);
    }

    return ret;
}
