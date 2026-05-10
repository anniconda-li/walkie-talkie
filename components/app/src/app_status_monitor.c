/**
 * @file app_status_monitor.c
 * @brief UI 状态监听业务。
 */
#include "app_status_monitor.h"

#include "app_common.h"
#include "app_ui.h"
#include "osal_task.h"
#include "service_battery.h"
#include "service_network.h"

#include <stddef.h>

static const char *TAG = "app_status_monitor";

static volatile int s_started = 0;
static volatile int s_network_ready = 0;

static int app_status_monitor_csq_to_bars(const service_network_status_t *status)
{
    /* 网络未 ready 或 CSQ 未知时显示 0 格，避免 UI 给出误导性信号。 */
    if (status == NULL || status->at_ready != 1 || status->sim_ready != 1 ||
        status->link_state != 1 || status->rssi < 0 || status->rssi == 99) {
        return 0;
    }

    if (status->rssi <= 9) {
        return 1;
    }
    if (status->rssi <= 14) {
        return 2;
    }
    if (status->rssi <= 19) {
        return 3;
    }
    return 4;
}

static void app_status_monitor_battery_task(void *arg)
{
    (void)arg;

    while (1) {
        /* service 层已经完成 ADC 滤波和百分比映射，UI 只消费百分比。 */
        int voltage_mv = 0;
        int percent = 0;
        if (service_battery_get_status(&voltage_mv, &percent) == 0) {
            (void)app_ui_set_battery_level(percent);
        }
        osal_delay_ms(1000u);
    }
}

static void app_status_monitor_network_task(void *arg)
{
    (void)arg;

    while (1) {
        service_network_status_t status;
        if (service_network_get_status(&status) == 0) {
            int bars = app_status_monitor_csq_to_bars(&status);
            (void)app_ui_set_network_state(bars);
            s_network_ready = bars > 0 ? 1 : 0;
        } else {
            (void)app_ui_set_network_state(0);
            s_network_ready = 0;
            /* 初始化失败或掉线后允许后台继续重试，不阻塞主业务启动。 */
            (void)service_network_init(NULL);
        }

        osal_delay_ms(3000u);
    }
}

int app_status_monitor_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = osal_task_create("biz_battery",
                               app_status_monitor_battery_task,
                               NULL,
                               4096u,
                               5u,
                               NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "电量监听任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_network",
                           app_status_monitor_network_task,
                           NULL,
                           4096u,
                           4u,
                           NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "网络监听任务启动失败, ret=%d", ret);
        return ret;
    }

    s_started = 1;
    return 0;
}

int app_status_monitor_network_ready(void)
{
    return s_network_ready;
}
