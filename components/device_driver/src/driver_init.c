/**
 * @file driver_init.c
 * @brief Driver 层统一初始化实现。
 */
#include "driver_init.h"

#include "bsp_i2c.h"
#include "bsp_i2s.h"
#include "bsp_uart.h"
#include "driver_audio_board.h"
#include "driver_battery.h"
#include "driver_camera.h"
#include "driver_es7210.h"
#include "driver_es8311.h"
#include "driver_lcd.h"
#include "driver_inmp441.h"
#include "driver_max98357a.h"
#include "driver_ml307c.h"
#include "driver_pca9557.h"
#include "driver_wifi.h"
#include "osal_task.h"

static const char *TAG = "driver_init";

#define DRIVER_INIT_ML307C_TIMEOUT_MS 5000u
#define DRIVER_INIT_ML307C_SOCKET_ID  1u

int driver_network_init(void)
{
    int ret = 0;

#if DRIVER_INIT_NETWORK == DRIVER_INIT_NETWORK_WIFI
    driver_wifi_config_t wifi_cfg = {
        .ssid = DRIVER_INIT_WIFI_SSID,
        .password = DRIVER_INIT_WIFI_PASSWORD,
    };
    ret = driver_wifi_init(&wifi_cfg);
    if (ret != 0) {
        DRIVER_LOGW(TAG, "WiFi 驱动初始化失败, ret=%d", ret);
    }
#elif DRIVER_INIT_NETWORK == DRIVER_INIT_NETWORK_ML307C
    {
        driver_ml307c_bsp_ops_t ml307c_bsp_ops = {
            .uart_write = bsp_uart_write,
            .uart_read = bsp_uart_read,
            .delay_ms = osal_delay_ms,
            .get_tick_ms = osal_get_tick_ms,
        };
        ml307c_config_t ml307c_cfg = {
            .timeout_ms = DRIVER_INIT_ML307C_TIMEOUT_MS,
            .socket_id = DRIVER_INIT_ML307C_SOCKET_ID,
        };

        int ml307c_ret = driver_ml307c_init(&ml307c_bsp_ops, &ml307c_cfg);
        if (ml307c_ret != 0) {
            DRIVER_LOGW(TAG, "ML307C 驱动初始化失败, ret=%d", ml307c_ret);
            if (ret == 0) {
                ret = ml307c_ret;
            }
        }
    }
#else
#error "Unsupported DRIVER_INIT_NETWORK selection"
#endif

    return ret;
}

static int driver_init_audio_es(void)
{
    int ret = driver_audio_board_codec_power_on();
    if (ret != 0) {
        DRIVER_LOGE(TAG, "音频 codec 供电失败, ret=%d", ret);
        return ret;
    }

    driver_es7210_bsp_ops_t es7210_bsp_ops = {
        .i2c_write_reg = bsp_i2c_write_reg,
        .i2c_read_reg = bsp_i2c_read_reg,
        .i2s_read = bsp_i2s_read,
    };
    ret = driver_es7210_init(&es7210_bsp_ops);
    if (ret != 0) {
        return ret;
    }

    driver_es8311_bsp_ops_t es8311_bsp_ops = {
        .i2c_write_reg = bsp_i2c_write_reg,
        .i2c_read_reg = bsp_i2c_read_reg,
        .i2s_write = bsp_i2s_write,
    };
    return driver_es8311_init(&es8311_bsp_ops);
}

static int driver_init_audio_i2s(void)
{
    driver_inmp441_bsp_ops_t inmp441_bsp_ops = {
        .i2s_read = bsp_i2s_read,
    };
    int ret = driver_inmp441_init(&inmp441_bsp_ops);
    if (ret != 0) {
        return ret;
    }

    driver_max98357a_bsp_ops_t max98357a_bsp_ops = {
        .i2s_write = bsp_i2s_write,
    };
    return driver_max98357a_init(&max98357a_bsp_ops);
}

int driver_audio_init(void)
{
    int ret = 0;

#if DRIVER_INIT_AUDIO == DRIVER_INIT_AUDIO_ES
    ret = driver_init_audio_es();
    if (ret != 0) {
        DRIVER_LOGE(TAG, "ES 音频驱动初始化失败, ret=%d", ret);
        return ret;
    }
#elif DRIVER_INIT_AUDIO == DRIVER_INIT_AUDIO_I2S
    ret = driver_init_audio_i2s();
    if (ret != 0) {
        DRIVER_LOGE(TAG, "I2S 音频驱动初始化失败, ret=%d", ret);
        return ret;
    }
#else
#error "Unsupported DRIVER_INIT_AUDIO selection"
#endif

    return 0;
}

int driver_init(void)
{
    int ret = driver_audio_init();
    if (ret != 0) {
        return ret;
    }

    driver_pca9557_bsp_ops_t pca9557_bsp_ops = {
        .get_i2c_bus_handle = bsp_i2c_get_bus_handle,
        .i2c_write_reg = bsp_i2c_write_reg,
        .i2c_read_reg = bsp_i2c_read_reg,
    };
    ret = driver_pca9557_init(&pca9557_bsp_ops);
    if (ret != 0) {
        DRIVER_LOGE(TAG, "PCA9557 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = driver_lcd_init();
    if (ret != 0) {
        DRIVER_LOGE(TAG, "LCD/Touch 初始化失败, ret=%d", ret);
        return ret;
    }

#if DRIVER_INIT_ENABLE_CAMERA
    /*
     * 摄像头依赖 I2C 和 PCA9557 camera power-down 控制，因此放在 PCA9557
     * 初始化之后。默认关闭，避免未接摄像头或测试阶段引脚未固定时影响主业务。
     */
    ret = driver_camera_init();
    if (ret != 0) {
        DRIVER_LOGW(TAG, "摄像头驱动初始化失败，摄像头业务将不可用, ret=%d", ret);
    }
#endif

    ret = driver_battery_init();
    if (ret != 0) {
        DRIVER_LOGE(TAG, "电池采样驱动初始化失败, ret=%d", ret);
        return ret;
    }

    ret = driver_network_init();
    if (ret != 0) {
        DRIVER_LOGW(TAG, "网络驱动初始化失败，业务将以未联网状态继续, ret=%d", ret);
    }

    return 0;
}
