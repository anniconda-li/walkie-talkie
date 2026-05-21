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
 * ESP32-S3 版本使用项目内裁剪后的 LVGL C 字体，
 * 不依赖运行时加载 TTF/OTF。
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
