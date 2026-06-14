/**
 * @file main.c
 * @brief Power key hold/shutdown test entry.
 */

#include "d_power_control.h"
#include "osal_log.h"
#include "osal_task.h"

static const char *TAG = "main_power_test";

static void main_fatal(const char *stage, int ret)
{
    OSAL_LOGE(TAG, "%s失败, ret=%d", stage, ret);
    while (1) {
        osal_delay_ms(1000u);
    }
}

void app_main(void)
{
    OSAL_LOGI(TAG, "电源保持/长按关机测试开始");

    int ret = d_power_control_init();
    if (ret != 0) {
        main_fatal("电源控制初始化", ret);
    }

    ret = d_power_control_start_monitor();
    if (ret != 0) {
        main_fatal("电源键监测任务启动", ret);
    }

    while (1) {
        OSAL_LOGI(TAG, "电源测试运行中，松开开机键后再次长按 2 秒应关机");
        osal_delay_ms(1000u);
    }
}
