/**
 * @file service_screen.c
 * @brief ESP 平台屏幕服务实现。
 */
#include "service_screen.h"

#include "esp_lvgl_port.h"
#include "service_common.h"

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
static service_screen_device_ops_t s_screen_ops;
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

static int service_screen_ops_is_valid(const service_screen_device_ops_t *ops)
{
    if (ops == NULL ||
        ops->is_initialized == NULL ||
        ops->get_panel_io == NULL ||
        ops->get_panel == NULL ||
        ops->get_touch == NULL ||
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
    if (cfg->device_ops.get_panel_io() == NULL ||
        cfg->device_ops.get_panel() == NULL ||
        cfg->device_ops.get_touch() == NULL) {
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
        .io_handle = s_screen_ops.get_panel_io(),
        .panel_handle = s_screen_ops.get_panel(),
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
        .disp = s_screen_display,
        .handle = s_screen_ops.get_touch(),
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
    return s_screen_ops_ready != 0u ? s_screen_ops.hres : 0u;
}

uint16_t service_screen_get_vres(void)
{
    return s_screen_ops_ready != 0u ? s_screen_ops.vres : 0u;
}

lv_display_t *service_screen_get_display(void)
{
    return s_screen_display;
}

lv_indev_t *service_screen_get_touch_indev(void)
{
    return s_screen_touch;
}
