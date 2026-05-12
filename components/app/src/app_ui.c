/**
 * @file app_ui.c
 * @brief 应用主界面实现。
 */
#include "app_ui.h"

#include "app_config.h"
#include "service_screen.h"
#include "ui.h"
#include "ui_shell.h"

#include <stdint.h>

/**
 * @brief UI 日志标签。
 */
static const char *TAG = "app_ui";

static int s_ui_created = 0;

int app_ui_create(void)
{
    if (s_ui_created) {
        APP_LOGI(TAG, "应用 UI 已创建");
        return 0;
    }

    if (service_screen_get_display() == NULL) {
        APP_LOGE(TAG, "UI 创建失败: 屏幕服务未初始化");
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 创建失败: LVGL 加锁超时");
        return -2;
    }

    ui_init();
    s_ui_created = 1;
    service_screen_unlock();

    APP_LOGI(TAG, "应用 UI 创建完成");
    return 0;
}

int app_ui_set_network_state(int state)
{
    if (!s_ui_created) {
        return -1;
    }

    if (state < 0) {
        state = 0;
    } else if (state > 4) {
        state = 4;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 网络状态更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_shell_set_signal_level((uint8_t)state);
    service_screen_unlock();
    return 0;
}

int app_ui_set_battery_level(int percent)
{
    if (!s_ui_created) {
        return -1;
    }

    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 电量更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_shell_set_battery_level((uint8_t)percent);
    service_screen_unlock();
    return 0;
}

int app_ui_set_intercom_state(int state)
{
    (void)state;
    return s_ui_created ? 0 : -1;
}

int app_ui_set_record_state(int state)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 录音状态更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_shell_set_battery_level((uint8_t)(20 + ((state & 0x03) * 20)));
    service_screen_unlock();
    return 0;
}
