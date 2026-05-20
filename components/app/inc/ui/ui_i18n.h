/**
 * @file ui_i18n.h
 * @brief UI 多语言文本接口。
 */
#ifndef UI_I18N_H
#define UI_I18N_H

#include <stdbool.h>

/** @brief UI 固定使用中文，保留语言类型用于兼容旧接口。 */
typedef enum {
    UI_LANG_CN = 0 /**< 中文。 */
} ui_lang_t;

/**
 * @brief UI 文本 ID。
 */
typedef enum {
    UI_TEXT_APP_INTERCOM = 0,       /**< 对讲应用名。 */
    UI_TEXT_APP_CAMERA,             /**< 相机应用名。 */
    UI_TEXT_APP_AI,                 /**< AI 应用名。 */
    UI_TEXT_APP_SETTINGS,           /**< 设置应用名。 */
    UI_TEXT_INTERCOM_PTT,           /**< PTT 按钮文本。 */
    UI_TEXT_INTERCOM_CHANNEL,       /**< 频道文本。 */
    UI_TEXT_CAMERA_LIVE,            /**< 相机实时预览文本。 */
    UI_TEXT_CAMERA_LOCKED,          /**< 相机冻结预览文本。 */
    UI_TEXT_CAMERA_CAPTURE,         /**< 拍照文本。 */
    UI_TEXT_CAMERA_UPLOAD,          /**< 上传文本。 */
    UI_TEXT_CAMERA_RETAKE,          /**< 重拍文本。 */
    UI_TEXT_CAMERA_HOME,            /**< 返回 AI 页面文本。 */
    UI_TEXT_AI_IDLE,                /**< AI 空闲文本。 */
    UI_TEXT_AI_LISTENING,           /**< AI 录音中文本。 */
    UI_TEXT_AI_ASK,                 /**< AI 提问按钮文本。 */
    UI_TEXT_AI_NO_NETWORK,          /**< AI 无网络提示。 */
    UI_TEXT_AI_IMAGE_UPLOAD_FAILED, /**< 图片上传失败提示。 */
    UI_TEXT_AI_QUESTION_FAILED,     /**< AI 提问失败提示。 */
    UI_TEXT_AI_REPLY_FAILED,        /**< AI 回复接收失败提示。 */
    UI_TEXT_SETTINGS_BRIGHTNESS,    /**< 亮度文本。 */
    UI_TEXT_SETTINGS_VOLUME,        /**< 音量文本。 */
    UI_TEXT_SETTINGS_SERVER,        /**< 服务器文本。 */
    UI_TEXT_SETTINGS_IP,            /**< IP 文本。 */
    UI_TEXT_SETTINGS_PORT,          /**< 端口文本。 */
    UI_TEXT_COUNT                   /**< 文本 ID 数量。 */
} ui_text_id_t;

/**
 * @brief 兼容旧接口，当前 UI 固定中文。
 *
 * @param[in] lang 目标语言。
 */
void ui_i18n_set_language(ui_lang_t lang);

/**
 * @brief 获取 UI 当前语言。
 *
 * @return 当前语言。
 */
ui_lang_t ui_i18n_get_language(void);

/**
 * @brief 兼容旧语言判断接口。
 *
 * @return 固定返回 false。
 */
bool ui_i18n_is_english(void);

/**
 * @brief 获取指定文本 ID 对应的当前语言文本。
 *
 * @param[in] id 文本 ID。
 * @return 文本字符串指针。
 */
const char * ui_i18n_text(ui_text_id_t id);

#endif /* UI_I18N_H */
