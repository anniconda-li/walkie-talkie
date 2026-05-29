/**
 * @file app_boot_status.h
 * @brief 开机阶段状态记录接口。
 */
#ifndef APP_BOOT_STATUS_H
#define APP_BOOT_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_BOOT_STAGE_WDRIVER = 0,
    APP_BOOT_STAGE_SCREEN,
    APP_BOOT_STAGE_AUDIO,
    APP_BOOT_STAGE_BATTERY,
    APP_BOOT_STAGE_NETWORK,
    APP_BOOT_STAGE_CAMERA,
    APP_BOOT_STAGE_UI,
    APP_BOOT_STAGE_RUNTIME,
    APP_BOOT_STAGE_COUNT,
} app_boot_stage_t;

typedef enum {
    APP_BOOT_STATE_PENDING = 0,
    APP_BOOT_STATE_RUNNING,
    APP_BOOT_STATE_OK,
    APP_BOOT_STATE_WARN,
    APP_BOOT_STATE_ERROR,
} app_boot_state_t;

typedef struct {
    app_boot_state_t state;
    int code;
} app_boot_stage_status_t;

void app_boot_status_reset(void);
void app_boot_status_set(app_boot_stage_t stage, app_boot_state_t state, int code);
app_boot_stage_status_t app_boot_status_get(app_boot_stage_t stage);
const char *app_boot_status_stage_name(app_boot_stage_t stage);
const char *app_boot_status_state_name(app_boot_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOOT_STATUS_H */
