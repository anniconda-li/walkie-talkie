#include "ui_i18n.h"

static ui_lang_t g_language = UI_LANG_EN;

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

void ui_i18n_set_language(ui_lang_t lang)
{
    if(lang >= UI_LANG_CN && lang <= UI_LANG_EN) {
        g_language = lang;
    }
}

ui_lang_t ui_i18n_get_language(void)
{
    return g_language;
}

bool ui_i18n_is_english(void)
{
    return g_language == UI_LANG_EN;
}

const char * ui_i18n_text(ui_text_id_t id)
{
    if(id < 0 || id >= UI_TEXT_COUNT) {
        return "";
    }

    return g_language == UI_LANG_EN ? g_text_en[id] : g_text_cn[id];
}
