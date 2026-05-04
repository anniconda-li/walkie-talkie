/**
 * @file bsp_lcd.h
 * @brief BSP LCD 显示与触摸驱动接口。
 *
 * 本驱动封装 ST7789 显示屏和 FT6336/FT5x06 触摸控制器的底层 ESP-IDF 句柄，
 * 对外只提供初始化、绘制、清屏、显示开关和触摸读取接口。
 */
#ifndef BSP_LCD_H
#define BSP_LCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD 水平分辨率。
 */
#define BSP_LCD_H_RES 320u

/**
 * @brief LCD 垂直分辨率。
 */
#define BSP_LCD_V_RES 240u

/**
 * @brief 触摸控制器最大触点数。
 */
#define BSP_LCD_TOUCH_MAX_POINTS 2u

/**
 * @brief LCD 触摸点数据。
 */
typedef struct {
    uint16_t x;        /**< X 坐标。 */
    uint16_t y;        /**< Y 坐标。 */
    uint16_t strength; /**< 触摸强度。 */
    uint8_t track_id;  /**< 触点跟踪 ID。 */
} bsp_lcd_touch_point_t;

/**
 * @brief 初始化 LCD 显示和触摸。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_init(void);

/**
 * @brief 初始化 ST7789 LCD 显示。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_display_init(void);

/**
 * @brief 初始化 FT6336/FT5x06 LCD 触摸。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_touch_init(void);

/**
 * @brief 释放 LCD 显示和触摸资源。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_deinit(void);

/**
 * @brief 设置 LCD 显示开关。
 *
 * @param[in] on 0 关闭显示，非 0 打开显示。
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_display_on(int on);

/**
 * @brief 绘制 RGB565 位图。
 *
 * @param[in] x_start 起始 X 坐标。
 * @param[in] y_start 起始 Y 坐标。
 * @param[in] x_end 结束 X 坐标，开区间。
 * @param[in] y_end 结束 Y 坐标，开区间。
 * @param[in] color_data RGB565 像素数据。
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_draw_bitmap(int x_start,
                        int y_start,
                        int x_end,
                        int y_end,
                        const void *color_data);

/**
 * @brief 使用 RGB565 颜色填充全屏。
 *
 * @param[in] color RGB565 颜色值。
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_fill_screen(uint16_t color);

/**
 * @brief 读取 LCD 触摸点。
 *
 * @param[out] points 触摸点输出数组。
 * @param[in] max_points 输出数组最大元素个数。
 * @param[out] point_num 实际触摸点数量。
 * @return 成功返回 0；失败返回负值。
 */
int bsp_lcd_read_touch(bsp_lcd_touch_point_t *points,
                       uint8_t max_points,
                       uint8_t *point_num);

#ifdef __cplusplus
}
#endif

#endif /* BSP_LCD_H */
