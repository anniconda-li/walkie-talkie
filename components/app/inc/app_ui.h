/**
 * @file app_ui.h
 * @brief 应用 UI 页面接口。
 *
 * UI 页面属于 app 层，只表达产品界面和业务状态，不直接访问 WDRIVER。
 */
#ifndef APP_UI_H
#define APP_UI_H

#include "ui_i18n.h"
#include "ui_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建应用主界面。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_create(void);

/**
 * @brief 更新网络状态显示。
 *
 * @param[in] state 网络状态值。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_network_state(int state);

/**
 * @brief 更新电量显示。
 *
 * @param[in] percent 电量百分比，范围 0-100。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_battery_level(int percent);

/**
 * @brief 更新对讲状态显示。
 *
 * @param[in] state 对讲状态值。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_intercom_state(int state);

/**
 * @brief 更新录音状态显示。
 *
 * @param[in] state 录音状态值。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_record_state(int state);

/**
 * @brief 设置 AI 回答框等待动画。
 *
 * @param[in] waiting 非 0 开启等待动画，0 关闭并恢复空闲文本。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_ai_waiting(int waiting);

/**
 * @brief 设置 AI 回答框提示文本。
 *
 * @param[in] text_id UI 文本 ID。
 * @return 成功返回 0；失败返回负值。
 */
int app_ui_set_ai_message(ui_text_id_t text_id);

int app_ui_settings_show_wlan_scan_result(const ui_settings_wifi_ap_t *items,
                                          uint16_t count,
                                          int ret);
int app_ui_settings_show_wifi_connect_result(int ret);
int app_ui_settings_show_4g_select_result(ui_settings_4g_status_t status, int ret);

#ifdef __cplusplus
}
#endif

#endif /* APP_UI_H */
