/**
 * @file ui_i18n.c
 * @brief 中文 UI 文本表。
 *
 * ## 设计
 * - 当前 UI 固定使用中文
 * - 所有 UI 文本存储在编译期静态数组中
 * - 保留旧语言接口为空操作，避免调用方断链
 *
 * ## 文本 ID 映射
 * 文本 ID 定义在 ui_i18n.h 的 ui_text_id_t 枚举中。
 * 数组索引必须与枚举值一一对应，顺序如有变更会导致显示错乱。
 *
 * ## 使用方式
 * ```c
 * lv_label_set_text(label, ui_i18n_text(UI_TEXT_AI_ASK));
 * // 返回音频图标
 * ```
 *
 * ## 局限性
 * - 不支持运行时切换语言
 * - 不支持运行时加载语言包（全部编译进固件）
 * - 字符串长度固定，不适用于超长文本（如 AI 回答）
 */
#include "ui_i18n.h"
#include "lvgl.h"

/* ==========================================================================
 * 中文文本表（必须与 ui_text_id_t 枚举顺序完全一致）
 * ========================================================================== */
static const char * const g_text_cn[UI_TEXT_COUNT] = {
    "对讲",
    "相机",
    "问答",
    "设置",
    LV_SYMBOL_AUDIO,
    "对讲频道",
    "实时预览",
    "画面已定格",
    LV_SYMBOL_IMAGE,
    LV_SYMBOL_UPLOAD,
    LV_SYMBOL_REFRESH,
    LV_SYMBOL_LEFT,
    "回答会显示在这里。",
    "正在聆听...",
    LV_SYMBOL_AUDIO,
    "无网络，请连接网络后再试。",
    "图片上传中，正在等待服务器处理...",
    "图片处理完成，可以提问了。",
    "图片上传失败，请稍后重试。",
    "提问失败，请稍后重试。",
    "接收回复失败，请稍后重试。",
    "音量大小",
    "固件版本"
};

/* ==========================================================================
 * 公开接口
 * ========================================================================== */

/**
 * @brief 兼容旧语言切换接口。
 *
 * 当前 UI 固定中文，传入值会被忽略。
 */
void ui_i18n_set_language(ui_lang_t lang)
{
    (void)lang;
}

/** @brief 获取当前语言设置。 */
ui_lang_t ui_i18n_get_language(void)
{
    return UI_LANG_CN;
}

/** @brief 兼容旧语言判断接口，固定返回 false。 */
bool ui_i18n_is_english(void)
{
    return false;
}

/**
 * @brief 获取指定文本 ID 的当前语言字符串。
 *
 * 这是 UI 层获取文本的统一入口。
 *
 * @param id 文本 ID（见 ui_text_id_t 枚举）。
 * @return 当前语言的字符串；ID 非法时返回空字符串 ""。
 */
const char * ui_i18n_text(ui_text_id_t id)
{
    if(id < 0 || id >= UI_TEXT_COUNT) {
        return "";
    }

    return g_text_cn[id];
}
