/**
 * @file ui_event.h
 * @brief UI 事件桥接和视图注册接口。
 */
#ifndef UI_EVENT_H
#define UI_EVENT_H

#include "lvgl.h"
#include "ui_i18n.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_SETTINGS_WIFI_AP_MAX 8u

typedef enum {
    UI_SETTINGS_NETWORK_NONE = 0,
    UI_SETTINGS_NETWORK_WLAN,
    UI_SETTINGS_NETWORK_4G,
} ui_settings_network_mode_t;

typedef enum {
    UI_SETTINGS_4G_OK = 0,
    UI_SETTINGS_4G_NO_SIM,
    UI_SETTINGS_4G_NO_AT,
    UI_SETTINGS_4G_NOT_REGISTERED,
    UI_SETTINGS_4G_UNAVAILABLE,
} ui_settings_4g_status_t;

typedef struct {
    char ssid[33];
    int rssi;
} ui_settings_wifi_ap_t;

/**
 * @brief UI 事件回调集合。
 *
 * UI 层只负责把按键、滑动和下拉框变化转换成事件；硬件和业务行为由 app 层
 * 注册回调处理。
 */
typedef struct {
    void (*intercom_channel_changed)(int32_t channel); /**< 对讲频道变化回调。 */
    void (*intercom_ptt_started)(int32_t channel);     /**< PTT 按下回调。 */
    void (*intercom_ptt_stopped)(int32_t channel);     /**< PTT 松开回调。 */
    void (*camera_entered)(void);                      /**< 相机页面进入回调。 */
    void (*camera_exited)(void);                       /**< 相机页面退出回调。 */
    void (*camera_capture_requested)(void);            /**< 相机拍照请求回调。 */
    void (*camera_upload_requested)(void);             /**< 相机上传请求回调。 */
    void (*camera_retake_requested)(void);             /**< 相机重拍请求回调。 */
    void (*ai_question_started)(void);                 /**< AI 问答录音开始回调。 */
    void (*ai_question_stopped)(void);                 /**< AI 问答录音停止回调。 */
    void (*settings_volume_changed)(int32_t value);    /**< 音量变化回调。 */
    ui_settings_network_mode_t (*settings_network_mode_get)(void); /**< 查询当前网络选择。 */
    void (*settings_wifi_scan_requested)(void);                    /**< WLAN 扫描请求。 */
    void (*settings_wifi_connect_requested)(const char *ssid,
                                            const char *password); /**< WLAN 连接请求。 */
    void (*settings_4g_select_requested)(void);                    /**< 4G 切换请求。 */
    int (*settings_wifi_ssid_get)(char *ssid, size_t size);        /**< 查询当前 WLAN SSID。 */
} ui_event_callbacks_t;

/**
 * @brief 对讲页面视图对象集合。
 */
typedef struct {
    lv_obj_t *channel_dec_button;  /**< 频道减少按钮。 */
    lv_obj_t *channel_inc_button;  /**< 频道增加按钮。 */
    lv_obj_t *channel_label;       /**< 当前频道标签。 */
    lv_obj_t *channel_hint_label;  /**< 频道提示标签。 */
    lv_obj_t *ptt_button;          /**< PTT 按钮。 */
    lv_obj_t *broadcast_rings[3];  /**< PTT 波纹动画对象。 */
    int32_t channel;               /**< 当前 UI 频道号。 */
} ui_intercom_view_t;

/**
 * @brief 相机页面视图对象集合。
 */
typedef struct {
    lv_obj_t *capture_icon;   /**< 拍照/重新预览按钮图标。 */
    lv_obj_t *upload_icon;    /**< 上传按钮图标。 */
    lv_obj_t *retake_icon;    /**< 返回按钮图标。 */
    lv_obj_t *capture_button; /**< 拍照/重新预览按钮。 */
    lv_obj_t *upload_button;  /**< 上传按钮。 */
    lv_obj_t *retake_button;  /**< Return 按钮。 */
    bool frozen;              /**< 预览是否冻结。 */
} ui_camera_view_t;

/**
 * @brief AI 页面视图对象集合。
 */
typedef struct {
    lv_obj_t *answer_label;  /**< AI 回答显示标签。 */
    lv_obj_t *camera_button; /**< 拍照按钮。 */
    lv_obj_t *camera_icon;   /**< 拍照按钮图标。 */
    lv_obj_t *ask_button;    /**< AI 问答按钮。 */
    lv_obj_t *ask_icon;      /**< AI 问答按钮图标。 */
    lv_obj_t *voice_bars[4]; /**< 录音动效柱。 */
    bool speaking;           /**< 是否处于录音动效状态。 */
} ui_ai_view_t;

/**
 * @brief 设置页面视图对象集合。
 */
typedef struct {
    lv_obj_t *volume_label;           /**< 音量标签。 */
    lv_obj_t *firmware_label;         /**< 固件版本标题标签。 */
    lv_obj_t *version_label;          /**< 固件版本号标签。 */
    lv_obj_t *volume_slider;          /**< 音量滑块。 */
    lv_obj_t *wlan_button;            /**< WLAN 模式选择框。 */
    lv_obj_t *cellular_button;        /**< 4G 模式选择框。 */
    lv_obj_t *wlan_ssid_label;        /**< WLAN 当前热点标签。 */
    lv_obj_t *network_status_label;   /**< 网络操作提示。 */
    lv_obj_t *wlan_page;              /**< WLAN 子页面。 */
    lv_obj_t *wlan_back_button;       /**< WLAN 子页面返回按钮。 */
    lv_obj_t *wlan_scan_button;       /**< WLAN 扫描按钮。 */
    lv_obj_t *wlan_list;              /**< WLAN 热点列表容器。 */
    lv_obj_t *password_dialog;        /**< WLAN 密码弹窗。 */
    lv_obj_t *password_title_label;    /**< WLAN 密码弹窗标题。 */
    lv_obj_t *password_textarea;      /**< WLAN 密码输入框。 */
    lv_obj_t *password_keyboard;      /**< WLAN 密码键盘。 */
    lv_obj_t *connect_button;         /**< WLAN 连接按钮。 */
    lv_obj_t *cancel_button;          /**< WLAN 取消连接按钮。 */
    lv_obj_t *connecting_spinner;     /**< WLAN 连接中动画。 */
    lv_obj_t *wlan_status_label;      /**< WLAN 页面状态提示。 */
    ui_settings_network_mode_t selected_network; /**< 当前 UI 选中的网络模式。 */
    char selected_ssid[33];           /**< 当前选中的 SSID。 */
} ui_settings_view_t;

/**
 * @brief 注册 UI 事件回调集合。
 *
 * @param[in] callbacks 回调集合；传入 NULL 时清空回调。
 */
void ui_event_set_callbacks(const ui_event_callbacks_t *callbacks);

/**
 * @brief 注册对讲页面视图对象。
 *
 * @param[in] view 对讲页面视图对象集合。
 */
void ui_event_register_intercom(ui_intercom_view_t *view);

/**
 * @brief 注册相机页面视图对象。
 *
 * @param[in] view 相机页面视图对象集合。
 */
void ui_event_register_camera(ui_camera_view_t *view);

/**
 * @brief 通知业务层相机页面已进入。
 */
void ui_event_notify_camera_entered(void);

/**
 * @brief 通知业务层相机页面已退出。
 */
void ui_event_notify_camera_exited(void);

/**
 * @brief 注册 AI 页面视图对象。
 *
 * @param[in] view AI 页面视图对象集合。
 */
void ui_event_register_ai(ui_ai_view_t *view);

/**
 * @brief 注销 AI 页面视图对象。
 *
 * @param[in] view AI 页面视图对象集合。
 */
void ui_event_unregister_ai(ui_ai_view_t *view);

/**
 * @brief 设置 AI 回答框等待动画状态。
 *
 * @param[in] waiting true 开启等待动画，false 关闭。
 */
void ui_event_set_ai_waiting(bool waiting);

/**
 * @brief 设置 AI 回答框提示文本。
 *
 * @param[in] text_id UI 文本 ID。
 */
void ui_event_set_ai_message(ui_text_id_t text_id);

/**
 * @brief 注册设置页面视图对象。
 *
 * @param[in] view 设置页面视图对象集合。
 */
void ui_event_register_settings(ui_settings_view_t *view);

/**
 * @brief 注销设置页面视图对象。
 *
 * @param[in] view 设置页面视图对象集合。
 */
void ui_event_unregister_settings(ui_settings_view_t *view);

void ui_event_settings_show_wlan_scan_result(const ui_settings_wifi_ap_t *items,
                                             uint16_t count,
                                             int ret);
void ui_event_settings_show_wifi_connect_result(int ret);
void ui_event_settings_show_4g_select_result(ui_settings_4g_status_t status, int ret);

#endif /* UI_EVENT_H */
