#ifndef UI_FONT_H
#define UI_FONT_H

#include "lvgl.h"

/*
 * ESP32-S3 版本不依赖 Windows 文件路径加载 TTF。
 * 当前先使用 LVGL 默认字体；后续需要中文显示时，在这里替换为裁剪后的 C 字体。
 */
void ui_font_init(void);
const lv_font_t * ui_font_normal(void);
const lv_font_t * ui_font_small(void);

#endif /* UI_FONT_H */
