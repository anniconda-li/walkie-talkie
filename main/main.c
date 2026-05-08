/**
 * @file main.c
 * @brief 电池电量 UI 测试入口。
 */

#include "app_ui.h"
#include "bsp.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_battery.h"
#include "service_screen.h"

/**
 * @brief 电池电量 UI 测试日志标签。
 */
static const char *TAG = "battery_ui_test";

#define BATTERY_TEST_REFRESH_MS    1000u

void app_main(void)
{
    OSAL_LOGI(TAG, "开始电池电量 UI 测试");

    int ret = bsp_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "BSP 基础资源初始化失败, ret=%d", ret);
        return;
    }

    ret = service_screen_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "屏幕服务初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = app_ui_create();
    if (ret != 0) {
        OSAL_LOGE(TAG, "应用 UI 创建失败, ret=%d", ret);
        (void)service_screen_deinit();
        (void)bsp_deinit();
        return;
    }

    ret = service_battery_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "电池服务初始化失败, ret=%d", ret);
        (void)service_screen_deinit();
        (void)bsp_deinit();
        return;
    }

    OSAL_LOGI(TAG, "电池电量 UI 测试启动完成");

    while (1) {
        int percent = 0;
        int voltage_mv = 0;

        ret = service_battery_get_status(&voltage_mv, &percent);
        if (ret == 0) {
            (void)app_ui_set_battery_level(percent);
            OSAL_LOGI(TAG, "电池 ADC 电压=%dmV, UI 电量=%d%%", voltage_mv, percent);
        } else {
            OSAL_LOGE(TAG, "电池状态读取失败, ret=%d", ret);
        }

        osal_delay_ms(BATTERY_TEST_REFRESH_MS);
    }
}
