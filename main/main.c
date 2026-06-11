/**
 * @file main.c
 * @brief 新板 LCD + Camera RGB565 直刷测试入口。
 */

#include "d_camera.h"
#include "d_lcd.h"
#include "d_pca9557.h"
#include "osal_log.h"
#include "osal_task.h"
#include "wdriver_i2c.h"
#include "wdriver_spi.h"

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "main_cam_lcd_test";

#define MAIN_PREVIEW_PERIOD_MS 10u
#define MAIN_FRAME_LOG_PERIOD  60u

static void main_fatal(const char *stage, int ret)
{
    OSAL_LOGE(TAG, "%s failed, ret=%d", stage, ret);
    while (1) {
        osal_delay_ms(1000u);
    }
}

static int main_init_pca9557(void)
{
    d_pca9557_wdriver_ops_t ops = {
        .get_i2c_bus_handle = wdriver_i2c_get_bus_handle,
        .i2c_write_reg = wdriver_i2c_write_reg,
        .i2c_read_reg = wdriver_i2c_read_reg,
    };

    return d_pca9557_init(&ops);
}

static void main_log_frame(uint32_t frame_count, const camera_fb_t *fb, int draw_ret)
{
    if ((frame_count % MAIN_FRAME_LOG_PERIOD) != 0u) {
        return;
    }

    OSAL_LOGI(TAG,
              "preview frame=%u size=%ux%u len=%u format=%d draw_ret=%d",
              (unsigned int)frame_count,
              (unsigned int)fb->width,
              (unsigned int)fb->height,
              (unsigned int)fb->len,
              (int)fb->format,
              draw_ret);
}

void app_main(void)
{
    OSAL_LOGI(TAG, "new board LCD + camera RGB565 test start");

    int ret = wdriver_i2c_init();
    if (ret != 0) {
        main_fatal("I2C init", ret);
    }

    ret = wdriver_spi_init();
    if (ret != 0) {
        main_fatal("SPI init", ret);
    }

    ret = main_init_pca9557();
    if (ret != 0) {
        main_fatal("PCA9557 init", ret);
    }

    ret = d_pca9557_set_camera_pwdn(PCA9557_LEVEL_LOW);
    if (ret != 0) {
        main_fatal("camera power enable", ret);
    }
    osal_delay_ms(100u);

    ret = d_lcd_display_init();
    if (ret != 0) {
        main_fatal("LCD display init", ret);
    }

    ret = d_lcd_display_on(1);
    if (ret != 0) {
        main_fatal("LCD display on", ret);
    }

    ret = d_camera_init();
    if (ret != 0) {
        main_fatal("camera init", ret);
    }

    ret = d_camera_set_rgb565_mode();
    if (ret != 0) {
        main_fatal("camera RGB565 mode", ret);
    }

    (void)d_lcd_fill_screen(0x0000u);

    uint32_t frame_count = 0u;
    while (1) {
        camera_fb_t *fb = d_camera_get_frame();
        if (fb == NULL) {
            OSAL_LOGE(TAG, "camera get frame failed");
            osal_delay_ms(100u);
            continue;
        }

        int draw_ret = -1;
        if (fb->format == PIXFORMAT_RGB565 &&
            fb->width <= d_lcd_H_RES &&
            fb->height <= d_lcd_V_RES &&
            fb->len >= ((size_t)fb->width * (size_t)fb->height * sizeof(uint16_t))) {
            const int x = ((int)d_lcd_H_RES - (int)fb->width) / 2;
            const int y = ((int)d_lcd_V_RES - (int)fb->height) / 2;
            draw_ret = d_lcd_draw_bitmap(x,
                                         y,
                                         x + (int)fb->width,
                                         y + (int)fb->height,
                                         fb->buf);
        } else {
            OSAL_LOGE(TAG,
                      "unexpected camera frame: format=%d size=%ux%u len=%u",
                      (int)fb->format,
                      (unsigned int)fb->width,
                      (unsigned int)fb->height,
                      (unsigned int)fb->len);
        }

        frame_count++;
        main_log_frame(frame_count, fb, draw_ret);
        d_camera_return_frame(fb);
        osal_delay_ms(MAIN_PREVIEW_PERIOD_MS);
    }
}
