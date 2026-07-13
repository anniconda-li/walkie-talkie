/**
 * @file app_ui.c
 * @brief 应用主界面实现。
 */
#include "app_ui.h"

#include "app_config.h"
#include "service_screen.h"
#include "ui.h"
#include "ui_event.h"
#include "ui_shell.h"
#include "lvgl.h"

#include <stdint.h>

/**
 * @brief UI 日志标签。
 */
static const char *TAG = "app_ui";

static int s_ui_created = 0;
static int s_screen_on = 0;

int app_ui_create(void)
{
    if (s_ui_created) {
        APP_LOGI(TAG, "应用 UI 已创建");
        return 0;
    }

    if (service_screen_is_initialized() != 1) {
        APP_LOGE(TAG, "UI 创建失败: 屏幕服务未初始化");
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 创建失败: LVGL 加锁超时");
        return -2;
    }

    ui_init();

    int ret = service_screen_display_on(1);
    if (ret != 0) {
        APP_LOGE(TAG, "UI 创建失败: 屏幕显示打开失败, ret=%d", ret);
        service_screen_unlock();
        return -3;
    }

    service_screen_unlock();
    s_ui_created = 1;
    s_screen_on = 1;

    APP_LOGI(TAG, "应用 UI 创建完成");
    return 0;
}

int app_ui_set_network_state(int state)
{
    return app_ui_set_network_status(state, 0);
}

int app_ui_set_network_status(int wifi_state, int cellular_state)
{
    if (!s_ui_created) {
        return -1;
    }

    if (wifi_state < 0) {
        wifi_state = 0;
    } else if (wifi_state > 4) {
        wifi_state = 4;
    }
    if (cellular_state < 0) {
        cellular_state = 0;
    } else if (cellular_state > 4) {
        cellular_state = 4;
    }

    if (service_screen_lock(100) != 0) {
        /* 周期性状态栏刷新可丢帧，避免相机预览占锁时刷失败日志。 */
        return 0;
    }

    ui_shell_set_signal_levels((uint8_t)wifi_state, (uint8_t)cellular_state);
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
        /* 周期性状态栏刷新可丢帧，避免相机预览占锁时刷失败日志。 */
        return 0;
    }

    ui_shell_set_battery_level((uint8_t)percent);
    service_screen_unlock();
    return 0;
}

int app_ui_show_power_dialog(void)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "关机确认弹窗显示失败: LVGL 加锁超时");
        return -2;
    }

    ui_shell_show_power_dialog();
    service_screen_unlock();
    return 0;
}

int app_ui_prepare_shutdown_blackout(void)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(500) != 0) {
        APP_LOGE(TAG, "关机黑屏刷新失败: LVGL 加锁超时");
        return -2;
    }

    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(screen, 0, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_refr_now(NULL);
    }

    service_screen_unlock();
    return 0;
}

int app_ui_set_screen_on(int on)
{
    if (!s_ui_created) {
        return -1;
    }

    on = on != 0 ? 1 : 0;
    if (s_screen_on == on) {
        return 0;
    }

    if (service_screen_lock(1000) != 0) {
        APP_LOGE(TAG, "屏幕亮灭切换失败: LVGL 加锁超时");
        return -2;
    }

    int ret = service_screen_display_on(on);
    service_screen_unlock();
    if (ret != 0) {
        APP_LOGE(TAG, "屏幕亮灭切换失败, on=%d, ret=%d", on, ret);
        return ret;
    }

    s_screen_on = on;
    APP_LOGI(TAG, "屏幕%s", on != 0 ? "亮屏" : "息屏");
    return 0;
}

int app_ui_toggle_screen_on(void)
{
    return app_ui_set_screen_on(s_screen_on ? 0 : 1);
}

int app_ui_set_intercom_state(int state)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(50) != 0) {
        return 0;
    }

    ui_event_set_intercom_state(state);
    service_screen_unlock();
    return 0;
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

int app_ui_set_ai_waiting(int waiting)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "AI 等待动画设置失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_ai_waiting(waiting != 0);
    service_screen_unlock();
    return 0;
}

int app_ui_set_ai_message(ui_text_id_t text_id)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "AI 提示文本设置失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_ai_message(text_id);
    service_screen_unlock();
    return 0;
}

int app_ui_set_ai_answer_text(const char *text)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "AI 回答文本设置失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_ai_answer_text(text);
    service_screen_unlock();
    return 0;
}

int app_ui_set_ai_audio_button_state(ui_ai_audio_btn_state_t state)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "AI 语音按钮状态设置失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_ai_audio_button_state(state);
    service_screen_unlock();
    return 0;
}

int app_ui_set_settings_volume(int32_t volume)
{
    if (!s_ui_created) {
        return -1;
    }

    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 音量更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_settings_volume(volume);
    service_screen_unlock();
    return 0;
}

int app_ui_set_settings_brightness(int32_t brightness)
{
    if (!s_ui_created) {
        return -1;
    }

    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 100) {
        brightness = 100;
    }

    if (service_screen_lock(100) != 0) {
        APP_LOGE(TAG, "UI 亮度更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_set_settings_brightness(brightness);
    service_screen_unlock();
    return 0;
}

int app_ui_settings_show_wlan_scan_result(const ui_settings_wifi_ap_t *items,
                                          uint16_t count,
                                          int ret)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(1000) != 0) {
        APP_LOGE(TAG, "WLAN 扫描结果更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_settings_show_wlan_scan_result(items, count, ret);
    service_screen_unlock();
    return 0;
}

int app_ui_settings_show_wifi_connect_result(int ret)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(1000) != 0) {
        APP_LOGE(TAG, "WLAN 连接结果更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_settings_show_wifi_connect_result(ret);
    service_screen_unlock();
    return 0;
}

int app_ui_settings_show_4g_select_result(ui_settings_4g_status_t status, int ret)
{
    if (!s_ui_created) {
        return -1;
    }

    if (service_screen_lock(1000) != 0) {
        APP_LOGE(TAG, "网络选择结果更新失败: LVGL 加锁超时");
        return -2;
    }

    ui_event_settings_show_4g_select_result(status, ret);
    service_screen_unlock();
    return 0;
}

int app_ui_set_ota_update(int available,
                          const char *version,
                          uint32_t size,
                          int min_battery,
                          int mandatory,
                          const char *release_notes)
{
    if (!s_ui_created || service_screen_lock(500u) != 0) {
        return -1;
    }
    ui_event_set_ota_update(available,
                            version,
                            size,
                            min_battery,
                            mandatory,
                            release_notes);
    service_screen_unlock();
    return 0;
}

int app_ui_ota_show(const char *message, int percent, int cancellable)
{
    if (!s_ui_created || service_screen_lock(500u) != 0) {
        return -1;
    }
    ui_event_ota_show(message, percent, cancellable);
    service_screen_unlock();
    return 0;
}

int app_ui_ota_finish_recovery(const char *message)
{
    if (!s_ui_created || service_screen_lock(1000u) != 0) {
        return -1;
    }
    ui_event_ota_finish_recovery(message);
    service_screen_unlock();
    return 0;
}
