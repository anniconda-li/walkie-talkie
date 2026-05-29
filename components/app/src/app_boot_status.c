/**
 * @file app_boot_status.c
 * @brief 开机阶段状态记录实现。
 */
#include "app_boot_status.h"

/** @brief 所有启动阶段的当前状态表，下标对应 app_boot_stage_t。 */
static app_boot_stage_status_t s_boot_status[APP_BOOT_STAGE_COUNT];

/** @brief 启动阶段显示名称表，下标对应 app_boot_stage_t。 */
static const char *const s_stage_names[APP_BOOT_STAGE_COUNT] = {
    "WDRIVER",
    "Screen",
    "Audio",
    "Battery",
    "Network",
    "Camera",
    "UI",
    "Runtime",
};

/**
 * @brief 重置所有启动阶段状态。
 *
 * 启动流程开始前调用，确保 UI 启动页不会显示上一次启动尝试留下的状态。
 */
void app_boot_status_reset(void)
{
    for (int i = 0; i < APP_BOOT_STAGE_COUNT; i++) {
        s_boot_status[i].state = APP_BOOT_STATE_PENDING;
        s_boot_status[i].code = 0;
    }
}

/**
 * @brief 更新单个启动阶段状态。
 *
 * 非法 stage 会被忽略，避免启动错误处理路径中再次触发异常。
 *
 * @param[in] stage 启动阶段。
 * @param[in] state 阶段状态。
 * @param[in] code 错误码或附加状态码。
 */
void app_boot_status_set(app_boot_stage_t stage, app_boot_state_t state, int code)
{
    if (stage < 0 || stage >= APP_BOOT_STAGE_COUNT) {
        return;
    }

    s_boot_status[stage].state = state;
    s_boot_status[stage].code = code;
}

/**
 * @brief 读取单个启动阶段状态。
 *
 * @param[in] stage 启动阶段。
 * @return 阶段状态快照；stage 非法时返回 ERROR/-1。
 */
app_boot_stage_status_t app_boot_status_get(app_boot_stage_t stage)
{
    if (stage < 0 || stage >= APP_BOOT_STAGE_COUNT) {
        return (app_boot_stage_status_t){
            .state = APP_BOOT_STATE_ERROR,
            .code = -1,
        };
    }

    return s_boot_status[stage];
}

/**
 * @brief 获取启动阶段显示名称。
 *
 * @param[in] stage 启动阶段。
 * @return 阶段名称；stage 非法时返回 "Unknown"。
 */
const char *app_boot_status_stage_name(app_boot_stage_t stage)
{
    if (stage < 0 || stage >= APP_BOOT_STAGE_COUNT) {
        return "Unknown";
    }

    return s_stage_names[stage];
}

/**
 * @brief 获取启动状态显示名称。
 *
 * @param[in] state 阶段状态。
 * @return 状态名称；state 非法时返回 "Unknown"。
 */
const char *app_boot_status_state_name(app_boot_state_t state)
{
    switch (state) {
    case APP_BOOT_STATE_PENDING:
        return "Pending";
    case APP_BOOT_STATE_RUNNING:
        return "Running";
    case APP_BOOT_STATE_OK:
        return "OK";
    case APP_BOOT_STATE_WARN:
        return "Warn";
    case APP_BOOT_STATE_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}
