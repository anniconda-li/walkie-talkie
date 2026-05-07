/**
 * @file bsp_lcd.c
 * @brief ST7789 LCD 显示与 FT6336/FT5x06 触摸 BSP 实现。
 */
#include "bsp_lcd.h"

#include "bsp_common.h"
#include "bsp_i2c.h"
#include "bsp_pca9557.h"
#include "bsp_spi.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"

#include <stdbool.h>

/**
 * @brief LCD 日志标签。
 */
static const char *TAG = "bsp_lcd";

/**
 * @brief LCD SPI 像素时钟。
 */
#define BSP_LCD_SPI_PCLK_HZ (40u * 1000u * 1000u)

/**
 * @brief LCD SPI 事务队列深度。
 */
#define BSP_LCD_SPI_TRANS_QUEUE_DEPTH 10

/**
 * @brief LCD 显示面板 IO 句柄。
 */
static esp_lcd_panel_io_handle_t s_lcd_panel_io = NULL;

/**
 * @brief LCD 显示面板句柄。
 */
static esp_lcd_panel_handle_t s_lcd_panel = NULL;

/**
 * @brief LCD 触摸面板 IO 句柄。
 */
static esp_lcd_panel_io_handle_t s_lcd_touch_io = NULL;

/**
 * @brief LCD 触摸控制器句柄。
 */
static esp_lcd_touch_handle_t s_lcd_touch = NULL;

/**
 * @brief LCD 背光使用的 PCA9557 句柄。
 */
static pca9557_handle_t s_lcd_pca9557 = NULL;

/**
 * @brief LCD 板级 PCA9557 I2C 访问接口。
 */
static pca9557_interface_t s_lcd_pca9557_itf = {
    .write_reg = pca9557_i2c_write_reg_impl,
    .read_reg = pca9557_i2c_read_reg_impl,
};

/**
 * @brief 将底层驱动错误码转换为 BSP 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int bsp_lcd_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/**
 * @brief 通过 PCA9557 准备 LCD 背光和板级相关 IO。
 *
 * 当前硬件中 PCA9557 IO5 连接 LCD 背光，高电平点亮；IO1 连接 OV-PWDN，
 * 输出低电平保持摄像头唤醒。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_lcd_board_io_init(void)
{
    if (s_lcd_pca9557 != NULL) {
        BSP_LOGI(TAG, "LCD 板级 IO 已初始化");
        return 0;
    }

    if (bsp_i2c_get_bus_handle() == NULL) {
        BSP_LOGE(TAG, "LCD 板级 IO 初始化失败: I2C 未初始化");
        return -1;
    }

    pca9557_config_t config = {
        .output_init = (uint8_t)(1u << PCA9557_PIN_5),
        .polarity_init = 0x00u,
        .direction_init = (uint8_t)~((1u << PCA9557_PIN_1) | (1u << PCA9557_PIN_5)),
    };

    s_lcd_pca9557 = pca9557_init(&config, &s_lcd_pca9557_itf);
    if (s_lcd_pca9557 == NULL) {
        BSP_LOGE(TAG, "LCD 板级 IO 初始化失败: PCA9557 初始化失败");
        return -2;
    }

    BSP_LOGI(TAG, "LCD 背光已打开, pca_io=5");
    return 0;
}

int bsp_lcd_display_init(void)
{
    if (s_lcd_panel != NULL) {
        BSP_LOGI(TAG, "LCD 显示已初始化");
        return 0;
    }

    int ret = bsp_lcd_board_io_init();
    if (ret != 0) {
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_spi_cfg = {
        .dc_gpio_num = BSP_LCD_DC_IO,
        .cs_gpio_num = BSP_LCD_CS_IO,
        .pclk_hz = BSP_LCD_SPI_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = BSP_LCD_SPI_TRANS_QUEUE_DEPTH,
    };

    ret = bsp_lcd_err_to_int(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)bsp_spi_get_host(),
                                                      &io_spi_cfg,
                                                      &s_lcd_panel_io));
    if (ret != 0) {
        BSP_LOGE(TAG, "LCD SPI panel IO 创建失败, ret=%d", ret);
        bsp_lcd_deinit();
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST_IO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ret = bsp_lcd_err_to_int(esp_lcd_new_panel_st7789(s_lcd_panel_io,
                                                      &panel_cfg,
                                                      &s_lcd_panel));
    if (ret != 0) {
        BSP_LOGE(TAG, "ST7789 面板创建失败, ret=%d", ret);
        bsp_lcd_deinit();
        return ret;
    }

    ret = bsp_lcd_err_to_int(esp_lcd_panel_reset(s_lcd_panel));
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_init(s_lcd_panel));
    }
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_invert_color(s_lcd_panel, false));
    }
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_swap_xy(s_lcd_panel, BSP_LCD_SWAP_XY != 0));
    }
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_mirror(s_lcd_panel,
                                                      BSP_LCD_MIRROR_X != 0,
                                                      BSP_LCD_MIRROR_Y != 0));
    }
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_set_gap(s_lcd_panel, 0, 0));
    }
    if (ret == 0) {
        ret = bsp_lcd_err_to_int(esp_lcd_panel_disp_on_off(s_lcd_panel, true));
    }
    if (ret != 0) {
        BSP_LOGE(TAG, "ST7789 面板初始化失败, ret=%d", ret);
        bsp_lcd_deinit();
        return ret;
    }

    BSP_LOGI(TAG, "LCD 显示初始化成功, res=%ux%u, pclk=%u",
             (unsigned int)BSP_LCD_H_RES,
             (unsigned int)BSP_LCD_V_RES,
             (unsigned int)BSP_LCD_SPI_PCLK_HZ);
    return 0;
}

int bsp_lcd_touch_init(void)
{
    if (s_lcd_touch != NULL) {
        BSP_LOGI(TAG, "LCD 触摸已初始化");
        return 0;
    }

    i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)bsp_i2c_get_bus_handle();
    if (i2c_bus == NULL) {
        BSP_LOGE(TAG, "LCD 触摸初始化失败: I2C 未初始化");
        return -1;
    }

    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    int ret = bsp_lcd_err_to_int(esp_lcd_new_panel_io_i2c(i2c_bus,
                                                          &touch_io_cfg,
                                                          &s_lcd_touch_io));
    if (ret != 0) {
        BSP_LOGE(TAG, "LCD 触摸 I2C panel IO 创建失败, ret=%d", ret);
        return ret;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST_IO,
        .int_gpio_num = BSP_LCD_TOUCH_INT_IO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = BSP_LCD_SWAP_XY != 0,
            .mirror_x = BSP_LCD_MIRROR_X != 0,
            .mirror_y = BSP_LCD_MIRROR_Y != 0,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    };

    ret = bsp_lcd_err_to_int(esp_lcd_touch_new_i2c_ft5x06(s_lcd_touch_io,
                                                          &touch_cfg,
                                                          &s_lcd_touch));
    if (ret != 0) {
        BSP_LOGE(TAG, "FT6336/FT5x06 触摸初始化失败, ret=%d", ret);
        esp_lcd_panel_io_del(s_lcd_touch_io);
        s_lcd_touch_io = NULL;
        return ret;
    }

    BSP_LOGI(TAG, "LCD 触摸初始化成功, int=%d", BSP_LCD_TOUCH_INT_IO);
    return 0;
}

int bsp_lcd_init(void)
{
    int ret = bsp_lcd_display_init();
    if (ret != 0) {
        return ret;
    }

    ret = bsp_lcd_touch_init();
    if (ret != 0) {
        bsp_lcd_deinit();
        return ret;
    }

    BSP_LOGI(TAG, "LCD 显示与触摸初始化完成");
    return 0;
}

int bsp_lcd_deinit(void)
{
    int ret = 0;

    if (s_lcd_touch != NULL) {
        int del_ret = bsp_lcd_err_to_int(esp_lcd_touch_del(s_lcd_touch));
        s_lcd_touch = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        BSP_LOGI(TAG, "LCD 触摸控制器已释放, ret=%d", del_ret);
    }

    if (s_lcd_touch_io != NULL) {
        int del_ret = bsp_lcd_err_to_int(esp_lcd_panel_io_del(s_lcd_touch_io));
        s_lcd_touch_io = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        BSP_LOGI(TAG, "LCD 触摸 IO 已释放, ret=%d", del_ret);
    }

    if (s_lcd_panel != NULL) {
        int del_ret = bsp_lcd_err_to_int(esp_lcd_panel_del(s_lcd_panel));
        s_lcd_panel = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        BSP_LOGI(TAG, "LCD 显示面板已释放, ret=%d", del_ret);
    }

    if (s_lcd_panel_io != NULL) {
        int del_ret = bsp_lcd_err_to_int(esp_lcd_panel_io_del(s_lcd_panel_io));
        s_lcd_panel_io = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        BSP_LOGI(TAG, "LCD 显示 IO 已释放, ret=%d", del_ret);
    }

    if (s_lcd_pca9557 != NULL) {
        pca9557_deinit(s_lcd_pca9557);
        s_lcd_pca9557 = NULL;
        BSP_LOGI(TAG, "LCD 板级 IO 已释放");
    }

    return ret;
}

int bsp_lcd_display_on(int on)
{
    if (s_lcd_panel == NULL) {
        BSP_LOGE(TAG, "LCD 显示开关失败: 显示未初始化");
        return -1;
    }

    int ret = bsp_lcd_err_to_int(esp_lcd_panel_disp_on_off(s_lcd_panel, on != 0));
    if (ret == 0) {
        BSP_LOGI(TAG, "LCD 显示%s", on != 0 ? "打开" : "关闭");
    } else {
        BSP_LOGE(TAG, "LCD 显示开关失败, ret=%d", ret);
    }

    return ret;
}

int bsp_lcd_draw_bitmap(int x_start,
                        int y_start,
                        int x_end,
                        int y_end,
                        const void *color_data)
{
    if (s_lcd_panel == NULL || color_data == NULL ||
        x_start < 0 || y_start < 0 ||
        x_end <= x_start || y_end <= y_start ||
        x_end > (int)BSP_LCD_H_RES || y_end > (int)BSP_LCD_V_RES) {
        BSP_LOGE(TAG, "LCD 画图参数无效, panel=%p, data=%p, area=(%d,%d)-(%d,%d)",
                 s_lcd_panel, color_data, x_start, y_start, x_end, y_end);
        return -1;
    }

    int ret = bsp_lcd_err_to_int(esp_lcd_panel_draw_bitmap(s_lcd_panel,
                                                           x_start,
                                                           y_start,
                                                           x_end,
                                                           y_end,
                                                           color_data));
    if (ret == 0) {
        BSP_LOGI(TAG, "LCD 画图成功, area=(%d,%d)-(%d,%d)", x_start, y_start, x_end, y_end);
    } else {
        BSP_LOGE(TAG, "LCD 画图失败, ret=%d", ret);
    }

    return ret;
}

int bsp_lcd_fill_screen(uint16_t color)
{
    if (s_lcd_panel == NULL) {
        BSP_LOGE(TAG, "LCD 清屏失败: 显示未初始化");
        return -1;
    }

    uint16_t line[BSP_LCD_H_RES];
    for (uint32_t i = 0; i < BSP_LCD_H_RES; i++) {
        line[i] = color;
    }

    for (uint32_t y = 0; y < BSP_LCD_V_RES; y++) {
        int ret = bsp_lcd_err_to_int(esp_lcd_panel_draw_bitmap(s_lcd_panel,
                                                               0,
                                                               (int)y,
                                                               (int)BSP_LCD_H_RES,
                                                               (int)y + 1,
                                                               line));
        if (ret != 0) {
            BSP_LOGE(TAG, "LCD 清屏失败, y=%u, ret=%d", (unsigned int)y, ret);
            return ret;
        }
    }

    BSP_LOGI(TAG, "LCD 清屏成功, color=0x%04X", (unsigned int)color);
    return 0;
}

int bsp_lcd_read_touch(bsp_lcd_touch_point_t *points,
                       uint8_t max_points,
                       uint8_t *point_num)
{
    if (s_lcd_touch == NULL || points == NULL || point_num == NULL || max_points == 0) {
        BSP_LOGE(TAG, "LCD 触摸读取参数无效, touch=%p, points=%p, point_num=%p, max=%u",
                 s_lcd_touch, points, point_num, (unsigned int)max_points);
        return -1;
    }

    esp_lcd_touch_point_data_t touch_data[BSP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t read_max = max_points;
    if (read_max > BSP_LCD_TOUCH_MAX_POINTS) {
        read_max = BSP_LCD_TOUCH_MAX_POINTS;
    }

    int ret = bsp_lcd_err_to_int(esp_lcd_touch_read_data(s_lcd_touch));
    if (ret != 0) {
        BSP_LOGE(TAG, "LCD 触摸原始数据读取失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_lcd_err_to_int(esp_lcd_touch_get_data(s_lcd_touch,
                                                    touch_data,
                                                    point_num,
                                                    read_max));
    if (ret != 0) {
        BSP_LOGE(TAG, "LCD 触摸坐标解析失败, ret=%d", ret);
        return ret;
    }

    for (uint8_t i = 0; i < *point_num; i++) {
        points[i].x = touch_data[i].x;
        points[i].y = touch_data[i].y;
        points[i].strength = touch_data[i].strength;
        points[i].track_id = touch_data[i].track_id;
    }

    if (*point_num > 0) {
        BSP_LOGI(TAG, "LCD 触摸读取成功, points=%u, x=%u, y=%u",
                 (unsigned int)*point_num,
                 (unsigned int)points[0].x,
                 (unsigned int)points[0].y);
    }

    return 0;
}

esp_lcd_panel_io_handle_t bsp_lcd_get_panel_io_handle(void)
{
    return s_lcd_panel_io;
}

esp_lcd_panel_handle_t bsp_lcd_get_panel_handle(void)
{
    return s_lcd_panel;
}

esp_lcd_touch_handle_t bsp_lcd_get_touch_handle(void)
{
    return s_lcd_touch;
}
