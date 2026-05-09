/**
 * @file ui_font.h
 * @brief UI 字体访问接口。
 */
#ifndef UI_FONT_H
#define UI_FONT_H

#include "lvgl.h"

/**
 * @brief 初始化 UI 字体资源。
 *
 * ESP32-S3 版本不依赖 Windows 文件路径加载 TTF；后续需要中文显示时，
 * 在这里替换为裁剪后的 LVGL C 字体。
 */
void ui_font_init(void);

/**
 * @brief 获取 UI 默认正文字体。
 *
 * @return LVGL 字体指针。
 */
const lv_font_t * ui_font_normal(void);

/**
 * @brief 获取 UI 小号字体。
 *
 * @return LVGL 字体指针。
 */
const lv_font_t * ui_font_small(void);

#endif /* UI_FONT_H */
