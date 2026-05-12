/**
 * @file app_status_monitor.c
 * @brief UI 状态监听业务——电池电量和网络信号周期性刷新。
 *
 * ## 模块职责
 * - 周期性采集电池 ADC 电压并换算为电量百分比，更新 UI 电池图标
 * - 周期性查询 4G/WiFi 网络状态（信号格数），更新 UI 信号图标
 * - 维护 s_network_ready 标志供心跳任务检测断线重连
 *
 * ## 任务列表
 * - biz_battery（优先级 5, 1s 周期）—— 读 ADC → 滤波 → 查表得百分比 → 更新 UI
 * - biz_network（优先级 4, 3s 周期）—— 查 AT 信号 → 换算格数 → 更新 UI + s_network_ready
 *
 * ## 调度方式
 * 两个任务都使用时间轮询（osal_delay_ms），不依赖 Notification。
 * 轮询周期较长（1s/3s）是为了降低 4G 模块的 AT 指令频率，
 * 因为每次 get_status 都会发多次 AT 指令（CSQ/CEREG/ISLINK），频繁查询会影响数据业务。
 */
#include "app_status_monitor.h"

#include "app_config.h"
#include "app_ui.h"
#include "osal_task.h"
#include "service_battery.h"
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

/**
 * @brief 将网络状态转换为 0-4 格信号图标。
 *
 * CSQ 值（0-31）映射规则：
 * - CSQ < 0 或 == 99（未知）→ 0 格
 * - CSQ 0-9   → 1 格
 * - CSQ 10-14 → 2 格
 * - CSQ 15-19 → 3 格
 * - CSQ 20-31 → 4 格
 *
 * 同时要求 AT 正常、SIM 正常、链路已连接，否则返回 0 格。
 *
 * @param status 网络状态快照。
 * @return 0-4 格；异常时返回 0。
 */
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

/**
 * @brief 电池电量轮询任务（1s 周期）。
 *
 * service_battery_get_status() 内部完成了 ADC 读取 → 一阶低通滤波 → 放电曲线查表 →
 * 5% 步进取整。本任务只将最终百分比推送给 UI。
 *
 * @param arg 未使用。
 */
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

/**
 * @brief 网络信号轮询任务（3s 周期）。
 *
 * ## 功能
 * 1. 查询 4G/WiFi 状态 → 换算信号格数 → 更新 UI
 * 2. 更新 s_network_ready 标志（供心跳任务和 app_intercom 使用）
 * 3. 查询失败时尝试重新初始化网络服务（容错恢复）
 *
 * ## 为什么 3 秒
 * ML307C 每次 get_status 会发 4 条 AT 指令（AT/CSQ/CEREG/ISLINK），
 * 频繁查询会影响 TCP/UDP 数据收发。3 秒间隔在 UI 体验和模块负载间取得平衡。
 *
 * @param arg 未使用。
 */
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

/**
 * @brief 启动状态监控后台任务。
 *
 * 创建两个轮询任务：电池（1s）和网络信号（3s）。
 * 开机后由 app_business_start() 调用。
 *
 * @return 成功返回 0；任务创建失败返回负值。
 */
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
