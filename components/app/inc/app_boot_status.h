/**
 * @file app_boot_status.h
 * @brief 开机阶段状态记录接口。
 *
 * 本模块只记录启动阶段、状态和错误码，不直接操作 UI。启动流程更新状态后，
 * UI 可通过 app_boot_status_get() 和名称接口刷新启动页显示。
 */
#ifndef APP_BOOT_STATUS_H
#define APP_BOOT_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机初始化阶段枚举。
 *
 * 枚举值作为状态表索引使用；实际执行顺序由 main.c 的启动编排决定。
 */
typedef enum {
    APP_BOOT_STAGE_WDRIVER = 0, /**< wdriver 基础资源初始化阶段。 */
    APP_BOOT_STAGE_SCREEN,      /**< 屏幕 driver/service 初始化阶段。 */
    APP_BOOT_STAGE_AUDIO,       /**< 音频 driver/service 初始化阶段。 */
    APP_BOOT_STAGE_BATTERY,     /**< 电池采样 driver/service 初始化阶段。 */
    APP_BOOT_STAGE_NETWORK,     /**< 网络 driver/service 初始化阶段。 */
    APP_BOOT_STAGE_CAMERA,      /**< 可选摄像头 driver/service 初始化阶段。 */
    APP_BOOT_STAGE_UI,          /**< UI 对象树创建阶段。 */
    APP_BOOT_STAGE_RUNTIME,     /**< app 运行期业务任务启动阶段。 */
    APP_BOOT_STAGE_COUNT,       /**< 启动阶段数量，必须放在最后。 */
} app_boot_stage_t;

/**
 * @brief 单个开机阶段的当前状态。
 */
typedef enum {
    APP_BOOT_STATE_PENDING = 0, /**< 尚未开始。 */
    APP_BOOT_STATE_RUNNING,     /**< 正在执行。 */
    APP_BOOT_STATE_OK,          /**< 已成功完成。 */
    APP_BOOT_STATE_WARN,        /**< 非致命异常，业务可继续启动。 */
    APP_BOOT_STATE_ERROR,       /**< 致命异常，启动流程应停在错误页。 */
} app_boot_state_t;

/**
 * @brief 单个开机阶段的状态快照。
 */
typedef struct {
    app_boot_state_t state; /**< 当前阶段状态。 */
    int code;               /**< 状态附带错误码；成功时通常为 0。 */
} app_boot_stage_status_t;

/**
 * @brief 重置所有开机阶段状态为 PENDING。
 */
void app_boot_status_reset(void);

/**
 * @brief 设置指定开机阶段状态。
 *
 * @param[in] stage 启动阶段。
 * @param[in] state 阶段状态。
 * @param[in] code 错误码或附加状态码。
 */
void app_boot_status_set(app_boot_stage_t stage, app_boot_state_t state, int code);

/**
 * @brief 获取指定开机阶段状态快照。
 *
 * @param[in] stage 启动阶段。
 * @return 阶段状态快照；stage 非法时返回 ERROR/-1。
 */
app_boot_stage_status_t app_boot_status_get(app_boot_stage_t stage);

/**
 * @brief 获取开机阶段的显示名称。
 *
 * @param[in] stage 启动阶段。
 * @return 阶段名称字符串；stage 非法时返回 "Unknown"。
 */
const char *app_boot_status_stage_name(app_boot_stage_t stage);

/**
 * @brief 获取开机状态的显示名称。
 *
 * @param[in] state 阶段状态。
 * @return 状态名称字符串；state 非法时返回 "Unknown"。
 */
const char *app_boot_status_state_name(app_boot_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOOT_STATUS_H */
