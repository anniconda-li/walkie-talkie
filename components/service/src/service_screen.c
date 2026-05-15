/**
 * @file service_screen.c
 * @brief ESP 平台屏幕服务实现。
 *
 * 屏幕 driver 负责初始化 LCD panel 和 touch 设备，service_screen 只负责把
 * 这些 driver 句柄接入 esp_lvgl_port，并向 app/UI 层提供 LVGL 加锁接口。
 */
#include "service_screen.h"

#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "service_config.h"

#include <stdbool.h>
#include <stdint.h>

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

/** @brief 当前绑定的屏幕 driver 能力函数表。 */
static service_screen_device_ops_t s_screen_ops;

/** @brief 屏幕 ops 是否已绑定成功。 */
static uint8_t s_screen_ops_ready = 0u;

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

/**
 * @brief 检查屏幕 service 所需的显示/触摸能力是否完整。
 *
 * @param[in] ops 待检查的屏幕能力函数表。
 * @return 有效返回 0；无效返回 -1。
 */
static int service_screen_ops_is_valid(const service_screen_device_ops_t *ops)
{
    /* 必须同时具备 panel IO、panel、touch 和分辨率，LVGL port 才能创建显示输入设备。 */
    if (ops == NULL ||
        ops->is_initialized == NULL ||
        ops->get_panel_io == NULL ||
        ops->get_panel == NULL ||
        ops->get_touch == NULL ||
        ops->draw_rgb565 == NULL ||
        ops->hres == 0u ||
        ops->vres == 0u) {
        return -1;
    }

    return 0;
}

int service_screen_init(const service_screen_config_t *cfg)
{
    if (s_screen_display != NULL) {
        SERVICE_LOGI(TAG, "屏幕服务已初始化");
        return 0;
    }

    if (cfg == NULL || service_screen_ops_is_valid(&cfg->device_ops) != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: 未提供屏幕能力");
        return -3;
    }

    if (cfg->device_ops.is_initialized() != 1) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: 下层屏幕 driver 未初始化");
        return -4;
    }
    void *panel_io = cfg->device_ops.get_panel_io();
    void *panel = cfg->device_ops.get_panel();
    void *touch = cfg->device_ops.get_touch();
    if (panel_io == NULL || panel == NULL || touch == NULL) {
        /* driver 已初始化但关键句柄为空时，说明底层 LCD/touch 初始化不完整。 */
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: 下层屏幕句柄无效");
        return -5;
    }

    s_screen_ops = cfg->device_ops;
    s_screen_ops_ready = 1u;

    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    int ret = service_screen_err_to_int(lvgl_port_init(&lvgl_port_cfg));
    if (ret != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败: LVGL port 初始化失败, ret=%d", ret);
        s_screen_ops_ready = 0u;
        return ret;
    }
    s_lvgl_port_inited = true;

    lvgl_port_display_cfg_t display_cfg = {
        /*
         * 使用 driver 已创建的 panel 句柄接入 LVGL。
         * buffer_size 按固定行数计算，避免在 service 层硬编码屏幕分辨率。
         */
        .io_handle = (esp_lcd_panel_io_handle_t)panel_io,
        .panel_handle = (esp_lcd_panel_handle_t)panel,
        .control_handle = NULL,
        .buffer_size = s_screen_ops.hres * SERVICE_SCREEN_DRAW_BUF_LINES,
        .double_buffer = false,
        .trans_size = 0,
        .hres = s_screen_ops.hres,
        .vres = s_screen_ops.vres,
        .monochrome = false,
        .rotation = {
            .swap_xy = s_screen_ops.swap_xy != 0u,
            .mirror_x = s_screen_ops.mirror_x != 0u,
            .mirror_y = s_screen_ops.mirror_y != 0u,
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
        /* touch 绑定到同一个 display，坐标变换由 LVGL port 和屏幕旋转参数处理。 */
        .disp = s_screen_display,
        .handle = (esp_lcd_touch_handle_t)touch,
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
                 (unsigned int)s_screen_ops.hres,
                 (unsigned int)s_screen_ops.vres);
    return 0;
}

int service_screen_deinit(void)
{
    int ret = 0;

    if (s_screen_touch != NULL) {
        /* 释放顺序与创建顺序相反：先 touch，再 display，最后 LVGL port。 */
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

    s_screen_ops = (service_screen_device_ops_t){0};
    s_screen_ops_ready = 0u;
    return ret;
}

int service_screen_lock(uint32_t timeout_ms)
{
    /* 所有 app/UI 修改 LVGL 对象前都应通过该接口加锁。 */
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

int service_screen_is_initialized(void)
{
    return s_screen_display != NULL ? 1 : 0;
}

int service_screen_draw_rgb565(int x, int y, int w, int h, const void *data)
{
    /*
     * 摄像头预览会绕开 LVGL image 对象直接推 RGB565 到 LCD。这里把直绘
     * 收敛在 screen service：app 不接触 LCD driver，同时 LCD 访问仍由
     * LVGL port 锁串行化，避免和 UI flush 同时操作 panel。
     */
    if (s_screen_ops_ready == 0u || s_screen_ops.draw_rgb565 == NULL) {
        SERVICE_LOGE(TAG, "RGB565 绘制失败: 屏幕服务未初始化");
        return -1;
    }
    if (data == NULL || x < 0 || y < 0 || w <= 0 || h <= 0) {
        SERVICE_LOGE(TAG, "RGB565 绘制参数无效, data=%p, area=(%d,%d,%d,%d)",
                     data, x, y, w, h);
        return -2;
    }
    if ((x + w) > (int)s_screen_ops.hres || (y + h) > (int)s_screen_ops.vres) {
        SERVICE_LOGE(TAG, "RGB565 绘制区域越界, area=(%d,%d,%d,%d), res=%ux%u",
                     x,
                     y,
                     w,
                     h,
                     (unsigned int)s_screen_ops.hres,
                     (unsigned int)s_screen_ops.vres);
        return -3;
    }

    int ret = service_screen_lock(100u);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "RGB565 绘制失败: 获取屏幕锁超时");
        return -4;
    }

    ret = s_screen_ops.draw_rgb565(x, y, w, h, data);
    service_screen_unlock();
    return ret;
}

uint16_t service_screen_get_hres(void)
{
    return s_screen_ops_ready != 0u ? s_screen_ops.hres : 0u;
}

uint16_t service_screen_get_vres(void)
{
    return s_screen_ops_ready != 0u ? s_screen_ops.vres : 0u;
}
