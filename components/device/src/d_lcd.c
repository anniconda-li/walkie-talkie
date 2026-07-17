/**
 * @file d_lcd.c
 * @brief ST7789 LCD 显示与 FT6336/FT5x06 触摸 WDRIVER 实现。
 */
#include "d_lcd.h"

#include "d_config.h"
#include "d_pca9557.h"
#include "wdriver_i2c.h"
#include "wdriver_spi.h"
#include "driver/gpio.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_heap_caps.h"

#include <stdbool.h>
#include <string.h>

/**
 * @brief LCD 日志标签。
 */
static const char *TAG = "d_lcd";

/**
 * @brief LCD SPI 像素时钟。
 */
#define d_lcd_SPI_PCLK_HZ (80u * 1000u * 1000u)

/**
 * @brief LCD SPI 事务队列深度。
 */
#define d_lcd_SPI_TRANS_QUEUE_DEPTH 10

/** @brief ST7789 列地址设置命令。 */
#define D_LCD_CMD_CASET 0x2A
/** @brief ST7789 行地址设置命令。 */
#define D_LCD_CMD_RASET 0x2B
/** @brief ST7789 显存写入命令。 */
#define D_LCD_CMD_RAMWR 0x2C

/**
 * @brief 摄像头预览整帧 polling RAMWR 开关。
 *
 * 当前关闭整帧写入，走多行分块 polling RAMWR，避免整帧 tx_param 在部分
 * LCD IO/驱动组合上不稳定。
 */
#ifndef D_LCD_PREVIEW_DIRECT_RAMWR
#define D_LCD_PREVIEW_DIRECT_RAMWR 0
#endif

/**
 * @brief 摄像头预览直绘 DMA 中转缓冲行数。
 *
 * camera frame 在 PSRAM，分块拷贝到内部 DMA buffer 后用 polling RAMWR 发屏。
 */
#define D_LCD_DMA_BOUNCE_LINES 16u
#define D_LCD_DMA_BOUNCE_MIN_LINES 1u

/**
 * @brief 直刷 RGB565 数据发送前交换字节。
 *
 * 摄像头 RGB565 帧当前已经匹配 LCD 直刷字节序，保持关闭。LVGL 自身的
 * swap_bytes 配置只适用于 LVGL draw buffer，不套用到 camera frame。
 */
#define D_LCD_PREVIEW_SWAP_RGB565_BYTES 0

/** @brief FT5x06 有效触摸阈值寄存器。 */
#define D_LCD_TOUCH_THRESHOLD_REG       0x80u
/** @brief 在组件默认值 70 基础上小幅降低，提高轻触识别率。 */
#define D_LCD_TOUCH_THRESHOLD           55u

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

/** @brief 摄像头预览直绘使用的内部 DMA 中转缓冲。 */
static uint8_t *s_lcd_dma_bounce_buf = NULL;

/** @brief 内部 DMA 中转缓冲大小。 */
static size_t s_lcd_dma_bounce_size = 0u;

/** @brief 当前可稳定分配的 DMA 中转缓冲行数。 */
static size_t s_lcd_dma_bounce_lines = D_LCD_DMA_BOUNCE_LINES;

#if D_LCD_PREVIEW_DIRECT_RAMWR
/** @brief 整帧 polling RAMWR 是否仍可尝试。 */
static uint8_t s_lcd_direct_ramwr_available = 1u;
#endif

static int d_lcd_err_to_int(int ret);
static int d_lcd_set_backlight(int on);
static int d_lcd_draw_bitmap_bounced(int x_start,
                                          int y_start,
                                          int x_end,
                                          int y_end,
                                          const void *color_data);
static void d_lcd_copy_rgb565_for_panel(uint8_t *dst, const uint8_t *src, size_t bytes);

/**
 * @brief 生成 ST7789 地址参数。
 *
 * ST7789 的 CASET/RASET 参数使用大端 16-bit 起止坐标，结束坐标为包含式。
 */
static void d_lcd_make_addr_param(uint8_t param[4], int start, int end)
{
    const uint16_t start_u16 = (uint16_t)start;
    const uint16_t end_u16 = (uint16_t)(end - 1);
    param[0] = (uint8_t)(start_u16 >> 8);
    param[1] = (uint8_t)(start_u16 & 0xFFu);
    param[2] = (uint8_t)(end_u16 >> 8);
    param[3] = (uint8_t)(end_u16 & 0xFFu);
}

/**
 * @brief 确保摄像头预览直绘 DMA 中转缓冲可用。
 *
 * @param[in] min_bytes 本次至少需要的字节数。
 * @return 成功返回 0；失败返回负值。
 */
static int d_lcd_ensure_dma_bounce(size_t min_bytes)
{
    if (s_lcd_dma_bounce_buf != NULL && s_lcd_dma_bounce_size >= min_bytes) {
        return 0;
    }

    if (s_lcd_dma_bounce_buf != NULL) {
        heap_caps_free(s_lcd_dma_bounce_buf);
        s_lcd_dma_bounce_buf = NULL;
        s_lcd_dma_bounce_size = 0u;
    }

    s_lcd_dma_bounce_buf = (uint8_t *)heap_caps_malloc(min_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_lcd_dma_bounce_buf == NULL) {
        D_LOGW(TAG,
               "LCD DMA 中转缓冲分配失败, bytes=%u, internal_free=%u, dma_largest=%u",
               (unsigned int)min_bytes,
               (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        return -1;
    }

    s_lcd_dma_bounce_size = min_bytes;
    D_LOGI(TAG, "LCD DMA 中转缓冲已分配, bytes=%u", (unsigned int)s_lcd_dma_bounce_size);
    return 0;
}

/**
 * @brief 将底层驱动错误码转换为 WDRIVER 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int d_lcd_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/** @brief 控制 LCD 背光，高电平点亮；GPIO_NUM_NC 表示背光接在 PCA9557 IO4。 */
static int d_lcd_set_backlight(int on)
{
    int ret = 0;

    if (d_lcd_BL_IO == GPIO_NUM_NC) {
        ret = d_pca9557_set_lcd_backlight(on);
        if (ret != 0) {
            D_LOGE(TAG, "LCD 背光控制失败, pca_io=4, ret=%d", ret);
        }
        return ret;
    }

    ret = d_lcd_err_to_int(gpio_set_level(d_lcd_BL_IO, on != 0));
    if (ret != 0) {
        D_LOGE(TAG, "LCD 背光控制失败, io=%d, ret=%d", d_lcd_BL_IO, ret);
    } else {
        D_LOGI(TAG, "LCD 背光%s, io=%d",
                 on != 0 ? "打开" : "关闭",
                 d_lcd_BL_IO);
    }
    return ret;
}

static void d_lcd_copy_rgb565_for_panel(uint8_t *dst, const uint8_t *src, size_t bytes)
{
#if D_LCD_PREVIEW_SWAP_RGB565_BYTES
    for (size_t i = 0u; i + 1u < bytes; i += 2u) {
        dst[i] = src[i + 1u];
        dst[i + 1u] = src[i];
    }
#else
    memcpy(dst, src, bytes);
#endif
}

int d_lcd_display_init(void)
{
    if (s_lcd_panel != NULL) {
        D_LOGI(TAG, "LCD 显示已初始化");
        return 0;
    }

    int ret = 0;
    if (d_lcd_BL_IO != GPIO_NUM_NC) {
        gpio_config_t bl_cfg = {
            .pin_bit_mask = 1ULL << d_lcd_BL_IO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = d_lcd_err_to_int(gpio_config(&bl_cfg));
    }
    if (ret == 0) {
        ret = d_lcd_set_backlight(0);
    }
    if (ret != 0) {
        D_LOGE(TAG, "LCD 显示初始化失败: 背光关闭失败, ret=%d", ret);
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_spi_cfg = {
        .dc_gpio_num = d_lcd_DC_IO,
        .cs_gpio_num = d_lcd_CS_IO,
        .pclk_hz = d_lcd_SPI_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = d_lcd_SPI_TRANS_QUEUE_DEPTH,
    };

    ret = d_lcd_err_to_int(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)wdriver_spi_get_host(),
                                                      &io_spi_cfg,
                                                      &s_lcd_panel_io));
    if (ret != 0) {
        D_LOGE(TAG, "LCD SPI panel IO 创建失败, ret=%d", ret);
        d_lcd_deinit();
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = d_lcd_RST_IO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ret = d_lcd_err_to_int(esp_lcd_new_panel_st7789(s_lcd_panel_io,
                                                      &panel_cfg,
                                                      &s_lcd_panel));
    if (ret != 0) {
        D_LOGE(TAG, "ST7789 面板创建失败, ret=%d", ret);
        d_lcd_deinit();
        return ret;
    }

    ret = d_lcd_err_to_int(esp_lcd_panel_reset(s_lcd_panel));
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_init(s_lcd_panel));
    }
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_invert_color(s_lcd_panel, false));
    }
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_swap_xy(s_lcd_panel, d_lcd_SWAP_XY != 0));
    }
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_mirror(s_lcd_panel,
                                                      d_lcd_MIRROR_X != 0,
                                                      d_lcd_MIRROR_Y != 0));
    }
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_set_gap(s_lcd_panel, 0, 0));
    }
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_disp_on_off(s_lcd_panel, true));
    }
    if (ret != 0) {
        D_LOGE(TAG, "ST7789 面板初始化失败, ret=%d", ret);
        d_lcd_deinit();
        return ret;
    }

    ret = d_lcd_fill_screen(0x0000u);
    if (ret != 0) {
        D_LOGE(TAG, "ST7789 面板初始化失败: 清黑屏失败, ret=%d", ret);
        d_lcd_deinit();
        return ret;
    }

    D_LOGI(TAG, "LCD 显示初始化成功, res=%ux%u, pclk=%u",
             (unsigned int)d_lcd_H_RES,
             (unsigned int)d_lcd_V_RES,
             (unsigned int)d_lcd_SPI_PCLK_HZ);
    return 0;
}

int d_lcd_touch_init(void)
{
    if (s_lcd_touch != NULL) {
        D_LOGI(TAG, "LCD 触摸已初始化");
        return 0;
    }

    if (wdriver_i2c_get_bus_handle() == NULL) {
        D_LOGE(TAG, "LCD 触摸初始化失败: I2C 未初始化");
        return -1;
    }

    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    /*
     * 当前工程为了兼容 ESP-IDF 5.3.x 的 esp-camera SCCB，WDRIVER I2C 使用
     * legacy driver/i2c.h。legacy esp_lcd I2C IO 的总线频率来自已经安装的
     * I2C driver，不允许在 panel IO config 里再次设置 scl_speed_hz。
     */
    touch_io_cfg.scl_speed_hz = 0;
    int ret = d_lcd_err_to_int(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)WDRIVER_I2C_PORT,
                                                          &touch_io_cfg,
                                                          &s_lcd_touch_io));
    if (ret != 0) {
        D_LOGE(TAG, "LCD 触摸 I2C panel IO 创建失败, ret=%d", ret);
        return ret;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = d_lcd_H_RES,
        .y_max = d_lcd_V_RES,
        .rst_gpio_num = d_lcd_TOUCH_RST_IO,
        /* 使用 LVGL 定时轮询，避免短触摸因 INT 边沿遗漏而完全丢失。 */
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = d_lcd_SWAP_XY != 0,
            .mirror_x = d_lcd_MIRROR_X != 0,
            .mirror_y = d_lcd_MIRROR_Y != 0,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    };

    ret = d_lcd_err_to_int(esp_lcd_touch_new_i2c_ft5x06(s_lcd_touch_io,
                                                          &touch_cfg,
                                                          &s_lcd_touch));
    if (ret != 0) {
        D_LOGE(TAG, "FT6336/FT5x06 触摸初始化失败, ret=%d", ret);
        esp_lcd_panel_io_del(s_lcd_touch_io);
        s_lcd_touch_io = NULL;
        return ret;
    }

    const uint8_t threshold = D_LCD_TOUCH_THRESHOLD;
    ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_touch_io,
                                                      D_LCD_TOUCH_THRESHOLD_REG,
                                                      &threshold,
                                                      sizeof(threshold)));
    if (ret != 0) {
        D_LOGE(TAG, "FT6336/FT5x06 触摸阈值设置失败, ret=%d", ret);
        (void)esp_lcd_touch_del(s_lcd_touch);
        s_lcd_touch = NULL;
        (void)esp_lcd_panel_io_del(s_lcd_touch_io);
        s_lcd_touch_io = NULL;
        return ret;
    }

    D_LOGI(TAG, "LCD 触摸初始化成功, mode=polling, threshold=%u",
           (unsigned int)D_LCD_TOUCH_THRESHOLD);
    return 0;
}

int d_lcd_init(void)
{
    int ret = d_lcd_display_init();
    if (ret != 0) {
        return ret;
    }

    ret = d_lcd_touch_init();
    if (ret != 0) {
        d_lcd_deinit();
        return ret;
    }

    D_LOGI(TAG, "LCD 显示与触摸初始化完成");
    return 0;
}

int d_lcd_deinit(void)
{
    int ret = 0;

    if (s_lcd_touch != NULL) {
        int del_ret = d_lcd_err_to_int(esp_lcd_touch_del(s_lcd_touch));
        s_lcd_touch = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        D_LOGI(TAG, "LCD 触摸控制器已释放, ret=%d", del_ret);
    }

    if (s_lcd_touch_io != NULL) {
        int del_ret = d_lcd_err_to_int(esp_lcd_panel_io_del(s_lcd_touch_io));
        s_lcd_touch_io = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        D_LOGI(TAG, "LCD 触摸 IO 已释放, ret=%d", del_ret);
    }

    if (s_lcd_panel != NULL) {
        int del_ret = d_lcd_err_to_int(esp_lcd_panel_del(s_lcd_panel));
        s_lcd_panel = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        D_LOGI(TAG, "LCD 显示面板已释放, ret=%d", del_ret);
    }

    if (s_lcd_panel_io != NULL) {
        int del_ret = d_lcd_err_to_int(esp_lcd_panel_io_del(s_lcd_panel_io));
        s_lcd_panel_io = NULL;
        if (ret == 0) {
            ret = del_ret;
        }
        D_LOGI(TAG, "LCD 显示 IO 已释放, ret=%d", del_ret);
    }

    if (s_lcd_dma_bounce_buf != NULL) {
        heap_caps_free(s_lcd_dma_bounce_buf);
        s_lcd_dma_bounce_buf = NULL;
        s_lcd_dma_bounce_size = 0u;
        s_lcd_dma_bounce_lines = D_LCD_DMA_BOUNCE_LINES;
        D_LOGI(TAG, "LCD DMA 中转缓冲已释放");
    }

    int bl_ret = d_lcd_set_backlight(0);
    if (ret == 0) {
        ret = bl_ret;
    }
    D_LOGI(TAG, "LCD 背光已关闭, io=%d, ret=%d", d_lcd_BL_IO, bl_ret);

    return ret;
}

int d_lcd_display_on(int on)
{
    if (s_lcd_panel == NULL) {
        D_LOGE(TAG, "LCD 显示开关失败: 显示未初始化");
        return -1;
    }

    int ret = 0;
    if (on != 0) {
        ret = d_lcd_set_backlight(1);
    }

    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_disp_on_off(s_lcd_panel, on != 0));
    }

    if (ret == 0 && on == 0) {
        ret = d_lcd_set_backlight(0);
    }

    if (ret == 0) {
        D_LOGI(TAG, "LCD 显示%s", on != 0 ? "打开" : "关闭");
    } else {
        D_LOGE(TAG, "LCD 显示开关失败, ret=%d", ret);
    }

    return ret;
}

int d_lcd_draw_bitmap(int x_start,
                        int y_start,
                        int x_end,
                        int y_end,
                        const void *color_data)
{
    if (s_lcd_panel == NULL || color_data == NULL ||
        x_start < 0 || y_start < 0 ||
        x_end <= x_start || y_end <= y_start ||
        x_end > (int)d_lcd_H_RES || y_end > (int)d_lcd_V_RES) {
        D_LOGE(TAG, "LCD 画图参数无效, panel=%p, data=%p, area=(%d,%d)-(%d,%d)",
                 s_lcd_panel, color_data, x_start, y_start, x_end, y_end);
        return -1;
    }

#if D_LCD_PREVIEW_DIRECT_RAMWR
    if (s_lcd_direct_ramwr_available == 0u) {
        return d_lcd_draw_bitmap_bounced(x_start, y_start, x_end, y_end, color_data);
    }

    uint8_t x_param[4];
    uint8_t y_param[4];
    d_lcd_make_addr_param(x_param, x_start, x_end);
    d_lcd_make_addr_param(y_param, y_start, y_end);

    /*
     * 使用 polling tx_param 直接写完整 RAMWR 数据。这样既避开 LVGL 的
     * on_color_trans_done 回调，又避免把预览帧拆成多条横带发送。
     */
    int ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                              D_LCD_CMD_CASET,
                                                              x_param,
                                                              sizeof(x_param)));
    if (ret == 0) {
        ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                              D_LCD_CMD_RASET,
                                                              y_param,
                                                              sizeof(y_param)));
    }
    if (ret == 0) {
        const size_t draw_bytes = (size_t)(x_end - x_start) *
                                  (size_t)(y_end - y_start) *
                                  sizeof(uint16_t);
        ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                              D_LCD_CMD_RAMWR,
                                                              color_data,
                                                              draw_bytes));
    }
    if (ret == 0) {
        return 0;
    }

    D_LOGW(TAG,
                "LCD 整帧 RAMWR 不可用，后续直接使用分块发送, ret=%d, area=(%d,%d)-(%d,%d)",
                ret,
                x_start,
                y_start,
                x_end,
                y_end);
    s_lcd_direct_ramwr_available = 0u;
    return d_lcd_draw_bitmap_bounced(x_start, y_start, x_end, y_end, color_data);
#else
    return d_lcd_draw_bitmap_bounced(x_start, y_start, x_end, y_end, color_data);
#endif
}

static int d_lcd_draw_bitmap_bounced(int x_start,
                                          int y_start,
                                          int x_end,
                                          int y_end,
                                          const void *color_data)
{
    const int width = x_end - x_start;
    const int height = y_end - y_start;
    const size_t line_bytes = (size_t)width * sizeof(uint16_t);
    size_t chunk_lines = s_lcd_dma_bounce_lines;
    if (chunk_lines > (size_t)height) {
        chunk_lines = (size_t)height;
    }
    if (chunk_lines == 0u) {
        return -2;
    }

    int ret = -1;
    while (chunk_lines >= D_LCD_DMA_BOUNCE_MIN_LINES) {
        ret = d_lcd_ensure_dma_bounce(line_bytes * chunk_lines);
        if (ret == 0) {
            s_lcd_dma_bounce_lines = chunk_lines;
            break;
        }

        if (chunk_lines == D_LCD_DMA_BOUNCE_MIN_LINES) {
            D_LOGE(TAG,
                   "LCD DMA 中转缓冲不可用, min_bytes=%u",
                   (unsigned int)(line_bytes * chunk_lines));
            return ret;
        }

        size_t next_lines = chunk_lines / 2u;
        if (next_lines < D_LCD_DMA_BOUNCE_MIN_LINES) {
            next_lines = D_LCD_DMA_BOUNCE_MIN_LINES;
        }
        D_LOGW(TAG,
               "LCD DMA 中转缓冲降级, lines=%u -> %u",
               (unsigned int)chunk_lines,
               (unsigned int)next_lines);
        chunk_lines = next_lines;
    }

    /*
     * 兼容回退路径：把 PSRAM 中的预览帧按较大的横带拷贝到内部 DMA buffer，
     * 每块发送完成后再复用 buffer。
     */
    const uint8_t *src = (const uint8_t *)color_data;
    int y = y_start;
    while (y < y_end) {
        const int remain_lines = y_end - y;
        const int draw_lines = remain_lines > (int)chunk_lines ? (int)chunk_lines : remain_lines;
        const size_t draw_bytes = line_bytes * (size_t)draw_lines;

        d_lcd_copy_rgb565_for_panel(s_lcd_dma_bounce_buf,
                                         src + ((size_t)(y - y_start) * line_bytes),
                                         draw_bytes);

        uint8_t x_param[4];
        uint8_t y_param[4];
        d_lcd_make_addr_param(x_param, x_start, x_end);
        d_lcd_make_addr_param(y_param, y, y + draw_lines);

        /*
         * 不使用 esp_lcd_panel_draw_bitmap()/tx_color。LVGL port 已经在同一个
         * panel IO 上注册 on_color_trans_done，camera 预览如果走 tx_color，
         * 会误触发 LVGL 的 flush_ready。这里全部用 tx_param 的 polling
         * 路径发送，避免触发 LVGL flush 回调。
         */
        ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                              D_LCD_CMD_CASET,
                                                              x_param,
                                                              sizeof(x_param)));
        if (ret == 0) {
            ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                                  D_LCD_CMD_RASET,
                                                                  y_param,
                                                                  sizeof(y_param)));
        }
        if (ret == 0) {
            ret = d_lcd_err_to_int(esp_lcd_panel_io_tx_param(s_lcd_panel_io,
                                                                  D_LCD_CMD_RAMWR,
                                                                  s_lcd_dma_bounce_buf,
                                                                  draw_bytes));
        }
        if (ret != 0) {
            D_LOGE(TAG,
                        "LCD 画图失败, ret=%d, area=(%d,%d)-(%d,%d)",
                        ret,
                        x_start,
                        y,
                        x_end,
                        y + draw_lines);
            return ret;
        }

        y += draw_lines;
    }

    return 0;
}

int d_lcd_fill_screen(uint16_t color)
{
    if (s_lcd_panel == NULL) {
        D_LOGE(TAG, "LCD 清屏失败: 显示未初始化");
        return -1;
    }

    uint16_t line[d_lcd_H_RES];
    for (uint32_t i = 0; i < d_lcd_H_RES; i++) {
        line[i] = color;
    }

    for (uint32_t y = 0; y < d_lcd_V_RES; y++) {
        int ret = d_lcd_err_to_int(esp_lcd_panel_draw_bitmap(s_lcd_panel,
                                                               0,
                                                               (int)y,
                                                               (int)d_lcd_H_RES,
                                                               (int)y + 1,
                                                               line));
        if (ret != 0) {
            D_LOGE(TAG, "LCD 清屏失败, y=%u, ret=%d", (unsigned int)y, ret);
            return ret;
        }
    }

    D_LOGI(TAG, "LCD 清屏成功, color=0x%04X", (unsigned int)color);
    return 0;
}

int d_lcd_read_touch(d_lcd_touch_point_t *points,
                       uint8_t max_points,
                       uint8_t *point_num)
{
    if (s_lcd_touch == NULL || points == NULL || point_num == NULL || max_points == 0) {
        D_LOGE(TAG, "LCD 触摸读取参数无效, touch=%p, points=%p, point_num=%p, max=%u",
                 s_lcd_touch, points, point_num, (unsigned int)max_points);
        return -1;
    }

    esp_lcd_touch_point_data_t touch_data[d_lcd_TOUCH_MAX_POINTS] = {0};
    uint8_t read_max = max_points;
    if (read_max > d_lcd_TOUCH_MAX_POINTS) {
        read_max = d_lcd_TOUCH_MAX_POINTS;
    }

    int ret = d_lcd_err_to_int(esp_lcd_touch_read_data(s_lcd_touch));
    if (ret != 0) {
        D_LOGE(TAG, "LCD 触摸原始数据读取失败, ret=%d", ret);
        return ret;
    }

    ret = d_lcd_err_to_int(esp_lcd_touch_get_data(s_lcd_touch,
                                                    touch_data,
                                                    point_num,
                                                    read_max));
    if (ret != 0) {
        D_LOGE(TAG, "LCD 触摸坐标解析失败, ret=%d", ret);
        return ret;
    }

    for (uint8_t i = 0; i < *point_num; i++) {
        points[i].x = touch_data[i].x;
        points[i].y = touch_data[i].y;
        points[i].strength = touch_data[i].strength;
        points[i].track_id = touch_data[i].track_id;
    }

    if (*point_num > 0) {
        D_LOGI(TAG, "LCD 触摸读取成功, points=%u, x=%u, y=%u",
                 (unsigned int)*point_num,
                 (unsigned int)points[0].x,
                 (unsigned int)points[0].y);
    }

    return 0;
}

int d_lcd_is_initialized(void)
{
    return (s_lcd_panel_io != NULL && s_lcd_panel != NULL && s_lcd_touch != NULL) ? 1 : 0;
}

esp_lcd_panel_io_handle_t d_lcd_get_panel_io_handle(void)
{
    return s_lcd_panel_io;
}

esp_lcd_panel_handle_t d_lcd_get_panel_handle(void)
{
    return s_lcd_panel;
}

esp_lcd_touch_handle_t d_lcd_get_touch_handle(void)
{
    return s_lcd_touch;
}
