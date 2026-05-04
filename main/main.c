/**
 * @file main.c
 * @brief 应用入口与外设基础测试流程。
 */

#include "bsp_i2c.h"
#include "bsp.h"
#include "bsp_i2s.h"
#include "bsp_inmp441.h"
#include "bsp_max98357a.h"
#include "bsp_ml307c.h"
#include "bsp_pca9557.h"
#include "bsp_uart.h"
#include "esp_log.h"
#include "osal_task.h"

#include <stdio.h>

/**
 * @brief 应用测试日志标签。
 */
static const char *TAG = "app_test";

/**
 * @brief 应用持有的 PCA9557 句柄。
 */
static pca9557_handle_t s_pca9557 = NULL;

/**
 * @brief 应用持有的 ML307C 句柄。
 */
static ml307c_handle_t s_ml307c = NULL;

/**
 * @brief 应用持有的 INMP441 句柄。
 */
static inmp441_handle_t s_inmp441 = NULL;

/**
 * @brief 应用持有的 MAX98357A 句柄。
 */
static max98357a_handle_t s_max98357a = NULL;

/**
 * @brief 打印通用测试步骤结果。
 *
 * @param[in] name 测试步骤名称。
 * @param[in] ret 测试步骤返回值。
 * @return 原样返回 ret，便于调用方继续判断。
 */
static int log_step_result(const char *name, int ret)
{
    if (ret == 0) {
        ESP_LOGI(TAG, "%s: 成功", name);
    } else {
        ESP_LOGE(TAG, "%s: 失败, ret=%d", name, ret);
    }

    return ret;
}

/**
 * @brief 初始化应用启动阶段需要长期持有的轻量外设对象。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int app_peripheral_init(void)
{
    if (log_step_result("BSP init", bsp_init()) != 0) {
        return -1;
    }

    pca9557_interface_t pca9557_itf = {
        .write_reg = pca9557_i2c_write_reg_impl,
        .read_reg = pca9557_i2c_read_reg_impl,
    };

    pca9557_config_t pca9557_cfg = {
        .output_init = 0x00,
        .polarity_init = 0x00,
        .direction_init = 0xFE,  /* P0 输出，P1-P7 输入 */
    };

    s_pca9557 = pca9557_init(&pca9557_cfg, &pca9557_itf);
    if (s_pca9557 == NULL) {
        ESP_LOGE(TAG, "PCA9557 初始化失败");
        return -2;
    }
    ESP_LOGI(TAG, "PCA9557 初始化完成");

    ml307c_config_t ml307c_cfg = {
        .timeout_ms = 3000,
    };

    ml307c_interface_t ml307c_itf = {
        .uart_write = ml307c_uart_write_impl,
        .uart_read = ml307c_uart_read_impl,
        .delay_ms = osal_delay_ms,
        .get_tick = osal_get_tick_ms,
    };

    s_ml307c = ml307c_init(&ml307c_cfg, &ml307c_itf);
    if (s_ml307c == NULL) {
        ESP_LOGE(TAG, "ML307C 初始化失败");
        return -3;
    }
    ESP_LOGI(TAG, "ML307C 初始化完成");

    inmp441_interface_t inmp441_itf = {
        .read = inmp441_i2s_read_impl,
    };

    s_inmp441 = inmp441_init(&inmp441_itf);
    if (s_inmp441 == NULL) {
        ESP_LOGE(TAG, "INMP441 初始化失败");
        return -4;
    }
    ESP_LOGI(TAG, "INMP441 初始化完成");

    max98357a_interface_t max98357a_itf = {
        .write = max98357a_i2s_write_impl,
    };

    s_max98357a = max98357a_init(&max98357a_itf);
    if (s_max98357a == NULL) {
        ESP_LOGE(TAG, "MAX98357A 初始化失败");
        return -5;
    }
    ESP_LOGI(TAG, "MAX98357A 初始化完成");

    return 0;
}

/**
 * @brief 执行 ML307C 模块基础功能测试。
 *
 * @param[in] dev ML307C 模块句柄。
 */
static void ml307c_basic_test(ml307c_handle_t dev)
{
    char imei[16] = {0};
    char iccid[21] = {0};
    char operator_name[17] = {0};
    int rssi = 99;

    if (log_step_result("AT check", ml307c_check_alive(dev)) != 0) {
        return;
    }

    log_step_result("SIM check", ml307c_check_sim(dev));

    if (ml307c_get_imei(dev, imei) == 0) {
        ESP_LOGI(TAG, "IMEI: %s", imei);
    } else {
        ESP_LOGW(TAG, "读取IMEI失败");
    }

    if (ml307c_get_iccid(dev, iccid) == 0) {
        ESP_LOGI(TAG, "ICCID: %s", iccid);
    } else {
        ESP_LOGW(TAG, "读取ICCID失败");
    }

    if (ml307c_get_signal(dev, &rssi) == 0) {
        ESP_LOGI(TAG, "信号强度: %d", rssi);
    } else {
        ESP_LOGW(TAG, "读取信号强度失败");
    }

    if (ml307c_get_operator(dev, operator_name) == 0) {
        ESP_LOGI(TAG, "运营商: %s", operator_name);
    } else {
        ESP_LOGW(TAG, "读取运营商失败");
    }

    log_step_result("Network register check", ml307c_check_network(dev));
}

/**
 * @brief 执行 PCA9557 基础功能测试。
 */
static void pca9557_basic_test(pca9557_handle_t pca9557)
{
    ESP_LOGI(TAG, "开始 PCA9557 基础测试");

    if (pca9557 == NULL) {
        ESP_LOGE(TAG, "PCA9557 句柄为空");
        return;
    }

    uint8_t input_value = 0;
    uint8_t output_value = 0;
    pca9557_level_t pin_level = PCA9557_LEVEL_LOW;

    log_step_result("PCA9557 set P0 output",
                    pca9557_set_pin_mode(pca9557, PCA9557_PIN_0, PCA9557_IO_OUTPUT));

    for (int i = 0; i < 4; i++) {
        pca9557_level_t level = (i % 2 == 0) ? PCA9557_LEVEL_HIGH : PCA9557_LEVEL_LOW;
        if (pca9557_set_pin_level(pca9557, PCA9557_PIN_0, level) == 0) {
            ESP_LOGI(TAG, "PCA9557 P0 输出: %s",
                     level == PCA9557_LEVEL_HIGH ? "HIGH" : "LOW");
        } else {
            ESP_LOGE(TAG, "PCA9557 P0 输出设置失败");
        }
        osal_delay_ms(500);
    }

    if (pca9557_read_output(pca9557, &output_value) == 0) {
        ESP_LOGI(TAG, "PCA9557 输出寄存器缓存: 0x%02X", output_value);
    }

    if (pca9557_read_input(pca9557, &input_value) == 0) {
        ESP_LOGI(TAG, "PCA9557 输入寄存器: 0x%02X", input_value);
    } else {
        ESP_LOGW(TAG, "PCA9557 输入寄存器读取失败");
    }

    if (pca9557_get_pin_level(pca9557, PCA9557_PIN_0, &pin_level) == 0) {
        ESP_LOGI(TAG, "PCA9557 P0 当前输入读数: %s",
                 pin_level == PCA9557_LEVEL_HIGH ? "HIGH" : "LOW");
    }

    ESP_LOGI(TAG, "PCA9557 基础测试完成");
}

/**
 * @brief ESP-IDF 应用入口。
 */
void app_main(void)
{
    if (log_step_result("Peripheral init", app_peripheral_init()) != 0) {
        return;
    }

    pca9557_basic_test(s_pca9557);

    ESP_LOGI(TAG, "开始 ML307C 基础测试");

    ml307c_basic_test(s_ml307c);

    ESP_LOGI(TAG, "ML307C 基础测试完成");
}
