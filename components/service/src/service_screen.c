/**
 * @file service_screen.c
 * @brief ESP 平台屏幕服务实现。
 */
#include "service_screen.h"

#include "bsp_lcd.h"
#include "esp_lvgl_port.h"
#include "service_common.h"

#include <stdbool.h>

/**
 * @brief 屏幕服务日志标签。
 */
static const char *TAG = "service_screen";

/**
 * @brief LVGL 单缓冲高度，单位为行。
 */
#define SERVICE_SCREEN_DRAW_BUF_LINES 20u

/**
 * @brief LVGL 显示对象。
 */
static lv_display_t *s_screen_display = NULL;

/**
 * @brief LVGL 触摸输入对象。
 */
static lv_indev_t *s_screen_touch = NULL;

/**
 * @brief LVGL port 是否已初始化。
 */
static bool s_lvgl_port_inited = false;

/**
 * @brief 将 ESP 错误码转换为通用 int 返回值。
 *
 * @param[in] ret ESP 错误码。
 * @return 成功返回 0；失败返回负值。
 */
static int service_screen_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

int service_screen_init(void)
{
    if (s_screen_display != NULL) {
        SERVICE_LOGI(TAG, "屏幕服务已初始化");
        return 0;
    }

    int ret = bsp_lcd_init();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: LCD 初始化失败, ret=%d", ret);
        return ret;
    }

    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ret = service_screen_err_to_int(lvgl_port_init(&lvgl_port_cfg));
    if (ret != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: LVGL port 初始化失败, ret=%d", ret);
        bsp_lcd_deinit();
        return ret;
    }
    s_lvgl_port_inited = true;

    lvgl_port_display_cfg_t display_cfg = {
        .io_handle = bsp_lcd_get_panel_io_handle(),
        .panel_handle = bsp_lcd_get_panel_handle(),
        .control_handle = NULL,
        .buffer_size = BSP_LCD_H_RES * SERVICE_SCREEN_DRAW_BUF_LINES,
        .double_buffer = false,
        .trans_size = 0,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = BSP_LCD_SWAP_XY != 0,
            .mirror_x = BSP_LCD_MIRROR_X != 0,
            .mirror_y = BSP_LCD_MIRROR_Y != 0,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    s_screen_display = lvgl_port_add_disp(&display_cfg);
    if (s_screen_display == NULL) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: LVGL display 创建失败");
        service_screen_deinit();
        return -1;
    }

    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_screen_display,
        .handle = bsp_lcd_get_touch_handle(),
        .scale = {
            .x = 1.0f,
            .y = 1.0f,
        },
    };

    s_screen_touch = lvgl_port_add_touch(&touch_cfg);
    if (s_screen_touch == NULL) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: LVGL touch 创建失败");
        service_screen_deinit();
        return -2;
    }

    SERVICE_LOGI(TAG, "屏幕服务初始化成功, res=%ux%u",
                 (unsigned int)BSP_LCD_H_RES,
                 (unsigned int)BSP_LCD_V_RES);
    return 0;
}

int service_screen_deinit(void)
{
    int ret = 0;

    if (s_screen_touch != NULL) {
        int del_ret = service_screen_err_to_int(lvgl_port_remove_touch(s_screen_touch));
        s_screen_touch = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        SERVICE_LOGI(TAG, "屏幕 touch 已释放, ret=%d", del_ret);
    }

    if (s_screen_display != NULL) {
        int del_ret = service_screen_err_to_int(lvgl_port_remove_disp(s_screen_display));
        s_screen_display = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        SERVICE_LOGI(TAG, "屏幕 display 已释放, ret=%d", del_ret);
    }

    if (s_lvgl_port_inited) {
        int del_ret = service_screen_err_to_int(lvgl_port_deinit());
        s_lvgl_port_inited = false;
        if (ret == 0) {
            ret = del_ret;
        }
        SERVICE_LOGI(TAG, "屏幕 LVGL port 已释放, ret=%d", del_ret);
    }

    int lcd_ret = bsp_lcd_deinit();
    if (ret == 0) {
        ret = lcd_ret;
    }

    return ret;
}

int service_screen_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_port_inited) {
        SERVICE_LOGE(TAG, "屏幕加锁失败: 服务未初始化");
        return -1;
    }

    return lvgl_port_lock(timeout_ms) ? 0 : -1;
}

void service_screen_unlock(void)
{
    if (s_lvgl_port_inited) {
        lvgl_port_unlock();
    }
}

uint16_t service_screen_get_hres(void)
{
    return (uint16_t)BSP_LCD_H_RES;
}

uint16_t service_screen_get_vres(void)
{
    return (uint16_t)BSP_LCD_V_RES;
}

lv_display_t *service_screen_get_display(void)
{
    return s_screen_display;
}

lv_indev_t *service_screen_get_touch_indev(void)
{
    return s_screen_touch;
}
