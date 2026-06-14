/**
 * @file d_init.c
 * @brief Driver 层统一初始化实现。
 */
#include "d_init.h"

#include "wdriver_i2c.h"
#include "wdriver_i2s.h"
#include "wdriver_uart.h"
#include "d_battery.h"
#include "d_camera.h"
#include "d_es7210.h"
#include "d_es8311.h"
#include "d_lcd.h"
#include "d_ml307c.h"
#include "d_pca9557.h"
#include "d_wifi.h"
#include "osal_task.h"

static const char *TAG = "d_init";

#define D_INIT_ML307C_TIMEOUT_MS 100000u
#define D_INIT_ML307C_SOCKET_ID  1u

int d_network_init(void)
{
    int ret = 0;

#if D_INIT_NETWORK == D_INIT_NETWORK_WIFI
    d_wifi_config_t wifi_cfg = {
        .ssid = D_INIT_WIFI_SSID,
        .password = D_INIT_WIFI_PASSWORD,
    };
    ret = d_wifi_init(&wifi_cfg);
    if (ret != 0) {
        D_LOGW(TAG, "WiFi 驱动初始化失败, ret=%d", ret);
    }
#elif D_INIT_NETWORK == D_INIT_NETWORK_ML307C
    {
        d_ml307c_wdriver_ops_t ml307c_wdriver_ops = {
            .uart_write = wdriver_uart_write,
            .uart_read = wdriver_uart_read,
            .delay_ms = osal_delay_ms,
            .get_tick_ms = osal_get_tick_ms,
        };
        ml307c_config_t ml307c_cfg = {
            .timeout_ms = D_INIT_ML307C_TIMEOUT_MS,
            .socket_id = D_INIT_ML307C_SOCKET_ID,
        };

        int ml307c_ret = d_ml307c_init(&ml307c_wdriver_ops, &ml307c_cfg);
        if (ml307c_ret != 0) {
            D_LOGW(TAG, "ML307C 驱动初始化失败, ret=%d", ml307c_ret);
            if (ret == 0) {
                ret = ml307c_ret;
            }
        }
    }
#else
#error "Unsupported D_INIT_NETWORK selection"
#endif

    return ret;
}

static int d_init_audio_es(void)
{
    d_es7210_wdriver_ops_t es7210_wdriver_ops = {
        .i2c_write_reg = wdriver_i2c_write_reg,
        .i2c_read_reg = wdriver_i2c_read_reg,
        .i2s_read = wdriver_i2s_read,
    };
    int ret = d_es7210_init(&es7210_wdriver_ops);
    if (ret != 0) {
        return ret;
    }

    d_es8311_wdriver_ops_t es8311_wdriver_ops = {
        .i2c_write_reg = wdriver_i2c_write_reg,
        .i2c_read_reg = wdriver_i2c_read_reg,
        .i2s_write = wdriver_i2s_write,
    };
    return d_es8311_init(&es8311_wdriver_ops);
}

int d_audio_init(void)
{
    int ret = d_init_audio_es();
    if (ret != 0) {
        D_LOGE(TAG, "ES 音频驱动初始化失败, ret=%d", ret);
        return ret;
    }

    return 0;
}

int d_screen_init(void)
{
    d_pca9557_wdriver_ops_t pca9557_wdriver_ops = {
        .get_i2c_bus_handle = wdriver_i2c_get_bus_handle,
        .i2c_write_reg = wdriver_i2c_write_reg,
        .i2c_read_reg = wdriver_i2c_read_reg,
    };
    int ret = d_pca9557_init(&pca9557_wdriver_ops);
    if (ret != 0) {
        D_LOGE(TAG, "PCA9557 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = d_lcd_init();
    if (ret != 0) {
        D_LOGE(TAG, "LCD/Touch 初始化失败, ret=%d", ret);
        return ret;
    }

    return 0;
}

int d_power_init(void)
{
    int ret = d_battery_init();
    if (ret != 0) {
        D_LOGE(TAG, "电池采样驱动初始化失败, ret=%d", ret);
    }

    return ret;
}

int d_optional_camera_init(void)
{
#if D_INIT_ENABLE_CAMERA
    int ret = d_camera_init();
    if (ret != 0) {
        D_LOGW(TAG, "摄像头驱动初始化失败，摄像头业务将不可用, ret=%d", ret);
    }
    return ret;
#else
    return -1;
#endif
}

int d_init(void)
{
    int ret = d_audio_init();
    if (ret != 0) {
        return ret;
    }

    ret = d_screen_init();
    if (ret != 0) {
        return ret;
    }

    ret = d_power_init();
    if (ret != 0) {
        return ret;
    }

#if D_INIT_ENABLE_CAMERA
    (void)d_optional_camera_init();
#endif

    ret = d_network_init();
    if (ret != 0) {
        D_LOGW(TAG, "网络驱动初始化失败，业务将以未联网状态继续, ret=%d", ret);
    }

    return 0;
}
