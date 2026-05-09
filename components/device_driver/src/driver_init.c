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
#include "driver_es7210.h"
#include "driver_es8311.h"
#include "driver_lcd.h"
#include "driver_inmp441.h"
#include "driver_max98357a.h"
#include "driver_ml307c.h"
#include "driver_wifi.h"
#include "osal_log.h"
#include "osal_task.h"

static const char *TAG = "driver_init";

#define DRIVER_INIT_ML307C_TIMEOUT_MS 5000u
#define DRIVER_INIT_ML307C_SOCKET_ID  1u

static int driver_init_network(const driver_init_config_t *cfg)
{
    if (cfg != NULL && cfg->network_backend == DRIVER_INIT_NETWORK_BACKEND_WIFI) {
        driver_wifi_config_t wifi_cfg = {
            .ssid = cfg->wifi_ssid,
            .password = cfg->wifi_password,
        };
        return driver_wifi_init(&wifi_cfg);
    }

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

    return driver_ml307c_init(&ml307c_bsp_ops, &ml307c_cfg);
}

static int driver_init_audio_input(const driver_init_config_t *cfg)
{
    if (cfg != NULL && cfg->audio_input_backend == DRIVER_INIT_AUDIO_INPUT_INMP441) {
        driver_inmp441_bsp_ops_t inmp441_bsp_ops = {
            .i2s_read = bsp_i2s_read,
        };
        return driver_inmp441_init(&inmp441_bsp_ops);
    }

    driver_es7210_bsp_ops_t es7210_bsp_ops = {
        .i2c_write_reg = bsp_i2c_write_reg,
        .i2c_read_reg = bsp_i2c_read_reg,
        .i2s_read = bsp_i2s_read,
    };
    return driver_es7210_init(&es7210_bsp_ops);
}

static int driver_init_audio_output(const driver_init_config_t *cfg)
{
    if (cfg != NULL && cfg->audio_output_backend == DRIVER_INIT_AUDIO_OUTPUT_MAX98357A) {
        driver_max98357a_bsp_ops_t max98357a_bsp_ops = {
            .i2s_write = bsp_i2s_write,
        };
        return driver_max98357a_init(&max98357a_bsp_ops);
    }

    driver_es8311_bsp_ops_t es8311_bsp_ops = {
        .i2c_write_reg = bsp_i2c_write_reg,
        .i2c_read_reg = bsp_i2c_read_reg,
        .i2s_write = bsp_i2s_write,
    };
    return driver_es8311_init(&es8311_bsp_ops);
}

int driver_init(const driver_init_config_t *cfg)
{
    if (cfg == NULL) {
        OSAL_LOGE(TAG, "Driver 初始化失败: cfg 为空");
        return -1;
    }

    if (cfg->audio_input_backend == DRIVER_INIT_AUDIO_INPUT_ES7210 ||
        cfg->audio_output_backend == DRIVER_INIT_AUDIO_OUTPUT_ES8311) {
        int power_ret = driver_audio_board_codec_power_on();
        if (power_ret != 0) {
            OSAL_LOGE(TAG, "音频 codec 供电失败, ret=%d", power_ret);
            return power_ret;
        }
    }

    int ret = driver_init_audio_input(cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "音频输入驱动初始化失败, backend=%d, ret=%d",
                  cfg->audio_input_backend, ret);
        return ret;
    }

    ret = driver_init_audio_output(cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "音频输出驱动初始化失败, backend=%d, ret=%d",
                  cfg->audio_output_backend, ret);
        return ret;
    }

    ret = driver_lcd_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "LCD/Touch 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = driver_battery_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "电池采样驱动初始化失败, ret=%d", ret);
        return ret;
    }

    ret = driver_init_network(cfg);
    if (ret != 0) {
        OSAL_LOGW(TAG, "网络驱动初始化失败，业务将以未联网状态继续, ret=%d", ret);
    }

    return 0;
}
