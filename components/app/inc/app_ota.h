/**
 * @file app_ota.h
 * @brief WiFi OTA 检查、下载、回滚与结果上报。
 */
#ifndef APP_OTA_H
#define APP_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_OTA_STATE_IDLE = 0,
    APP_OTA_STATE_CHECKING,
    APP_OTA_STATE_AVAILABLE,
    APP_OTA_STATE_PREPARING,
    APP_OTA_STATE_DOWNLOADING,
    APP_OTA_STATE_VERIFYING,
    APP_OTA_STATE_COMMITTING,
    APP_OTA_STATE_REBOOTING,
    APP_OTA_STATE_STOPPING,
    APP_OTA_STATE_FAILED,
    APP_OTA_STATE_RECOVERING,
} app_ota_state_t;

/** @brief 尽早启动新固件首次启动回滚守护；应在业务初始化前调用。 */
int app_ota_boot_guard_start(void);

/** @brief 基本自检通过后确认当前新固件有效。 */
int app_ota_boot_confirm(void);

/** @brief 启动后台检查、结果补报和升级任务。 */
int app_ota_start(void);

/** @brief 用户确认后请求安装当前缓存的新版本。 */
int app_ota_request_install(void);

/** @brief 用户二次确认后请求停止准备或下载。 */
int app_ota_request_cancel(void);

/** @brief OTA 是否已经进入全屏维护模式。 */
int app_ota_is_maintenance(void);

/** @brief 供维护编排轮询用户停止请求。 */
int app_ota_cancel_is_requested(void);

/** @brief 获取当前 OTA 状态。 */
app_ota_state_t app_ota_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_H */
