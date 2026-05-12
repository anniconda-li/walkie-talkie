/**
 * @file ui_i18n.c
 * @brief 第一版简易国际化——中英文文本切换。
 *
 * ## 设计
 * - 当前仅支持中文和英文两种语言
 * - 所有 UI 文本存储在编译期静态数组中（g_text_cn / g_text_en）
 * - 通过全局变量 g_language 控制当前语言
 * - ui_i18n_text(id) 根据 g_language 返回对应语言的字符串
 *
 * ## 文本 ID 映射
 * 文本 ID 定义在 ui_i18n.h 的 ui_text_id_t 枚举中。
 * 数组索引必须与枚举值一一对应，顺序如有变更会导致显示错乱。
 *
 * ## 使用方式
 * ```c
 * lv_label_set_text(label, ui_i18n_text(UI_TEXT_AI_ASK));
 * // 中文环境返回 "按住提问"，英文环境返回 "HOLD ASK"
 * ```
 *
 * ## 局限性
 * - 不支持动态添加语言（修改需要改代码重编译）
 * - 不支持运行时加载语言包（全部编译进固件）
 * - 字符串长度固定，不适用于超长文本（如 AI 回答）
 */
#include "ui_i18n.h"

/**
 * @brief 当前语言设置。
 *
 * 初始值 UI_LANG_EN（英文），开机后由设置页面控制。
 * 可通过 ui_i18n_set_language() 修改。
 */
static ui_lang_t g_language = UI_LANG_EN;

/* ==========================================================================
 * 中文文本表（必须与 ui_text_id_t 枚举顺序完全一致）
 * ========================================================================== */
static const char * const g_text_cn[UI_TEXT_COUNT] = {
    "对讲",
    "相机",
    "AI",
    "设置",
    "按住讲话",
    "对讲频道",
    "实时预览",
    "画面已定格",
    "拍照",
    "上传",
    "重拍",
    "AI 回答会显示在这里。",
    "正在聆听...",
    "按住提问",
    "屏幕亮度",
    "音量大小",
    "语言",
    "服务器",
    "IP   192.168.1.100",
    "端口 8080"
};

/* ==========================================================================
 * 英文文本表（必须与 ui_text_id_t 枚举顺序完全一致）
 * ========================================================================== */
static const char * const g_text_en[UI_TEXT_COUNT] = {
    "INTERCOM",
    "CAMERA",
    "AI",
    "SETTINGS",
    "PTT",
    "INTERCOM CHANNEL",
    "LIVE PREVIEW",
    "FRAME LOCKED",
    "CAP",
    "UPLOAD",
    "RETAKE",
    "AI response will appear here.",
    "Listening...",
    "HOLD ASK",
    "BRIGHTNESS",
    "VOLUME",
    "LANGUAGE",
    "SERVER",
    "IP   192.168.1.100",
    "PORT 8080"
};

/* ==========================================================================
 * 公开接口
 * ========================================================================== */

/**
 * @brief 设置当前语言。
 *
 * 非法值会被静默忽略，语言不会改变。
 *
 * @param lang 目标语言（UI_LANG_CN 或 UI_LANG_EN）。
 */
void ui_i18n_set_language(ui_lang_t lang)
{
    if(lang >= UI_LANG_CN && lang <= UI_LANG_EN) {
        g_language = lang;
    }
}

/** @brief 获取当前语言设置。 */
ui_lang_t ui_i18n_get_language(void)
{
    return g_language;
}

/** @brief 判断当前是否为英文。 */
bool ui_i18n_is_english(void)
{
    return g_language == UI_LANG_EN;
}

/**
 * @brief 获取指定文本 ID 的当前语言字符串。
 *
 * 这是 UI 层获取本地化文本的统一入口。
 * 所有需要显示文字的地方都通过此函数获取字符串，
 * 语言切换后只需刷新各 UI 元素即可。
 *
 * @param id 文本 ID（见 ui_text_id_t 枚举）。
 * @return 当前语言的字符串；ID 非法时返回空字符串 ""。
 */
const char * ui_i18n_text(ui_text_id_t id)
{
    if(id < 0 || id >= UI_TEXT_COUNT) {
        return "";
    }

    return g_language == UI_LANG_EN ? g_text_en[id] : g_text_cn[id];
}
