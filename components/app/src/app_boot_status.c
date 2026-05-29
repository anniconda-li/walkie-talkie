/**
 * @file app_boot_status.c
 * @brief 开机阶段状态记录实现。
 */
#include "app_boot_status.h"

static app_boot_stage_status_t s_boot_status[APP_BOOT_STAGE_COUNT];

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

void app_boot_status_reset(void)
{
    for (int i = 0; i < APP_BOOT_STAGE_COUNT; i++) {
        s_boot_status[i].state = APP_BOOT_STATE_PENDING;
        s_boot_status[i].code = 0;
    }
}

void app_boot_status_set(app_boot_stage_t stage, app_boot_state_t state, int code)
{
    if (stage < 0 || stage >= APP_BOOT_STAGE_COUNT) {
        return;
    }

    s_boot_status[stage].state = state;
    s_boot_status[stage].code = code;
}

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

const char *app_boot_status_stage_name(app_boot_stage_t stage)
{
    if (stage < 0 || stage >= APP_BOOT_STAGE_COUNT) {
        return "Unknown";
    }

    return s_stage_names[stage];
}

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
