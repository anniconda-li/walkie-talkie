/**
 * @file app_status_monitor.h
 * @brief UI 状态监听业务。
 *
 * 该模块启动后台轮询任务，把 service 层的电量和网络状态转换成 UI 可直接
 * 显示的百分比/信号格数，同时保存最近一次网络 ready 状态供对讲心跳使用。
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
 * 本接口由 app_business_start() 调用，重复调用会直接返回成功。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_status_monitor_start(void);

/**
 * @brief 查询最近一次网络状态是否可用于业务连接。
 *
 * 返回值来自后台网络状态任务的最近一次轮询结果，不会立即发起网络查询。
 *
 * @return 网络 ready 返回 1；否则返回 0。
 */
int app_status_monitor_network_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATUS_MONITOR_H */
