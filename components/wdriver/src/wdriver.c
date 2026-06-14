/**
 * @file wdriver.c
 * @brief WDRIVER 板级基础资源统一初始化实现。
 */
#include "wdriver.h"

#include "wdriver_config.h"
#include "wdriver_i2c.h"
#include "wdriver_i2s.h"
#include "wdriver_spi.h"
#include "wdriver_uart.h"

/**
 * @brief WDRIVER 日志标签。
 */
static const char *TAG = "wdriver";

int wdriver_init(void)
{
    int ret = wdriver_i2c_init();
    if (ret != 0) {
        WDRIVER_LOGE(TAG, "WDRIVER 初始化失败: I2C 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = wdriver_spi_init();
    if (ret != 0) {
        WDRIVER_LOGE(TAG, "WDRIVER 初始化失败: SPI 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = wdriver_uart_init();
    if (ret != 0) {
        WDRIVER_LOGE(TAG, "WDRIVER 初始化失败: UART 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = wdriver_i2s_init();
    if (ret != 0) {
        WDRIVER_LOGE(TAG, "WDRIVER 初始化失败: I2S 初始化失败, ret=%d", ret);
        return ret;
    }

    WDRIVER_LOGI(TAG, "WDRIVER 基础资源初始化完成");
    return 0;
}

int wdriver_deinit(void)
{
    int ret = 0;

    int i2s_ret = wdriver_i2s_deinit();
    if (ret == 0 && i2s_ret != 0) {
        ret = i2s_ret;
    }

    int spi_ret = wdriver_spi_deinit();
    if (ret == 0 && spi_ret != 0) {
        ret = spi_ret;
    }

    int i2c_ret = wdriver_i2c_deinit();
    if (ret == 0 && i2c_ret != 0) {
        ret = i2c_ret;
    }

    if (ret == 0) {
        WDRIVER_LOGI(TAG, "WDRIVER 基础资源释放完成");
    } else {
        WDRIVER_LOGE(TAG, "WDRIVER 基础资源释放存在失败, ret=%d", ret);
    }

    return ret;
}
