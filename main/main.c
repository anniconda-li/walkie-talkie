/**
 * @file main.c
 * @brief 应用入口与 ML307C 模块基础测试流程。
 */

#include "bsp_ml307c.h"
#include "bsp_uart.h"
#include "esp_log.h"
#include "osal_task.h"

#include <stdio.h>

/**
 * @brief ML307C 测试日志标签。
 */
static const char *TAG = "ml307c_test";

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
 * @brief ESP-IDF 应用入口。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "开始 ML307C 基础测试");

    if (bsp_uart_init() != 0) {
        ESP_LOGE(TAG, "UART 初始化失败");
        return;
    }

    ml307c_config_t cfg = {
        .timeout_ms = 3000,
    };

    ml307c_interface_t itf = {
        .uart_write = ml307c_uart_write_impl,
        .uart_read = ml307c_uart_read_impl,
        .delay_ms = osal_delay_ms,
        .get_tick = osal_get_tick_ms,
    };

    ml307c_handle_t ml307c = ml307c_init(&cfg, &itf);
    if (ml307c == NULL) {
        ESP_LOGE(TAG, "ML307C 初始化失败");
        return;
    }

    ml307c_basic_test(ml307c);
    ml307c_deinit(ml307c);

    ESP_LOGI(TAG, "ML307C 基础测试完成");
}
