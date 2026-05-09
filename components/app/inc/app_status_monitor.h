/**
 * @file app_status_monitor.h
 * @brief UI 状态监听业务。
 */
#ifndef APP_STATUS_MONITOR_H
#define APP_STATUS_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

int app_status_monitor_start(void);
int app_status_monitor_network_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATUS_MONITOR_H */
