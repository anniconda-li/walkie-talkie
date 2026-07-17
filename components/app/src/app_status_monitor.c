/**
 * @file app_status_monitor.c
 * @brief UI 状态监听业务——固定电量显示和网络信号周期性刷新。
 *
 * ## 模块职责
 * - 启动时按设备配置写入固定电量百分比
 * - 周期性查询 WiFi 网络状态（信号格数），更新 UI 信号图标
 * - 维护 s_network_ready 标志供心跳任务检测断线重连
 *
 * ## 任务列表
 * - biz_network（优先级 4, 3s 周期）—— 查网络信号 → 换算格数 → 更新 UI + s_network_ready
 *
 * ## 调度方式
 * 网络任务使用时间轮询（osal_delay_ms），不依赖 Notification。
 */
#include "app_status_monitor.h"

#include "app_config.h"
#include "app_network.h"
#include "app_ui.h"
#include "osal_task.h"
#include "service_network.h"

#include <stddef.h>

static const char *TAG = "app_status_monitor";

/** @brief 状态监控是否已启动。 */
static volatile int s_started = 0;

/**
 * @brief 最近一次网络状态查询结果：是否可用于业务连接。
 *
 * 由 biz_network 任务每 3 秒更新，供 biz_heartbeat 和 app_intercom 查询。
 * 1 = 网络就绪（信号 > 0 格且链路正常）；0 = 不可用。
 */
static volatile int s_network_ready = 0;

_Static_assert(APP_STATUS_BATTERY_PERCENT >= 0 && APP_STATUS_BATTERY_PERCENT <= 100,
               "fixed battery display percent must be in range 0..100");

/**
 * @brief 将网络状态转换为 0-4 格信号图标。
 *
 * WiFi driver 已把 RSSI 映射为 0-31 的通用信号值：
 * - 信号 < 0 或 == 99（未知）→ 0 格
 * - 信号 0-9   → 1 格
 * - 信号 10-14 → 2 格
 * - 信号 15-19 → 3 格
 * - 信号 20-31 → 4 格
 *
 * 同时要求链路已连接，否则返回 0 格。
 *
 * @param status 网络状态快照。
 * @return 0-4 格；异常时返回 0。
 */
static int app_status_monitor_network_to_bars(const service_network_status_t *status)
{
    /* 网络未 ready 或信号未知时显示 0 格，避免 UI 给出误导性信号。 */
    if (status == NULL || status->link_ready != 1 || status->rssi < 0 || status->rssi == 99) {
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

/**
 * @brief 网络信号轮询任务（3s 周期）。
 *
 * ## 功能
 * 1. 查询 WiFi 状态 → 换算信号格数 → 更新 UI
 * 2. 更新 s_network_ready 标志（供心跳任务和 app_intercom 使用）
 * 3. 查询失败时尝试重新初始化网络服务（容错恢复）
 *
 * @param arg 未使用。
 */
static void app_status_monitor_network_task(void *arg)
{
    (void)arg;

    while (1) {
        app_network_mode_t mode = app_network_get_mode();
        service_network_status_t status;

        if (service_network_get_status(&status) == 0) {
            int bars = app_status_monitor_network_to_bars(&status);
            if (mode == APP_NETWORK_MODE_4G) {
                (void)app_ui_set_network_status(0, bars);
            } else {
                (void)app_ui_set_network_status(bars, 0);
            }
            s_network_ready = bars > 0 ? 1 : 0;
        } else {
            (void)app_ui_set_network_status(0, 0);
            s_network_ready = 0;
            if (mode == APP_NETWORK_MODE_4G) {
                (void)app_network_recover();
            }
        }

        osal_delay_ms(3000u);
    }
}

/**
 * @brief 启动状态监控后台任务。
 *
 * 写入固定电量显示，并创建网络信号轮询任务（3s）。
 * 开机后由 app_business_start() 调用。
 *
 * @return 成功返回 0；任务创建失败返回负值。
 */
int app_status_monitor_start(void)
{
    if (s_started) {
        return 0;
    }

    (void)app_ui_set_battery_level(APP_STATUS_BATTERY_PERCENT);

    int ret = osal_task_create("biz_network",
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

/**
 * @brief 查询最近一次网络状态是否可用于业务连接。
 *
 * 由 biz_network 任务每 3 秒更新，供外部模块查询（如心跳任务的重连逻辑）。
 *
 * @return 1=网络就绪；0=不可用。
 */
int app_status_monitor_network_ready(void)
{
    return s_network_ready;
}
