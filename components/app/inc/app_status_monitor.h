/**
 * @file app_status_monitor.h
 * @brief UI 状态监听业务。
 */
#ifndef APP_STATUS_MONITOR_H
#define APP_STATUS_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 UI 状态监听后台任务。
 *
 * 包括电量 1 秒刷新任务和网络信号 3 秒刷新任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_status_monitor_start(void);

/**
 * @brief 查询最近一次网络状态是否可用于业务连接。
 *
 * @return 网络 ready 返回 1；否则返回 0。
 */
int app_status_monitor_network_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATUS_MONITOR_H */
