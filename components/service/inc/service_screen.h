/**
 * @file service_screen.h
 * @brief 屏幕服务接口。
 *
 * 本服务组合 LCD 显示、触摸和 LVGL 运行环境，对 app 层提供屏幕 UI 运行能力。
 */
#ifndef SERVICE_SCREEN_H
#define SERVICE_SCREEN_H

#include <stdint.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 屏幕服务依赖的下层显示与触摸能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层屏幕 driver 是否已初始化。 */
    esp_lcd_panel_io_handle_t (*get_panel_io)(void); /**< 获取显示 panel IO 句柄。 */
    esp_lcd_panel_handle_t (*get_panel)(void);       /**< 获取显示 panel 句柄。 */
    esp_lcd_touch_handle_t (*get_touch)(void);       /**< 获取触摸控制器句柄。 */
    uint16_t hres;          /**< 水平分辨率。 */
    uint16_t vres;          /**< 垂直分辨率。 */
    uint8_t swap_xy;        /**< 显示方向 swap_xy 配置。 */
    uint8_t mirror_x;       /**< 显示方向 mirror_x 配置。 */
    uint8_t mirror_y;       /**< 显示方向 mirror_y 配置。 */
} service_screen_device_ops_t;

/**
 * @brief 屏幕服务初始化配置。
 */
typedef struct {
    service_screen_device_ops_t device_ops; /**< 下层显示与触摸能力函数表。 */
} service_screen_config_t;

/**
 * @brief 初始化屏幕服务。
 *
 * 初始化时会复制下层屏幕能力函数表，并把 driver 已创建的 LCD panel/touch
 * 句柄接入 LVGL port。service 不负责初始化具体 LCD 或触摸芯片。
 *
 * @param[in] cfg 屏幕 service 初始化配置。
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_init(const service_screen_config_t *cfg);

/**
 * @brief 释放屏幕服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_deinit(void);

/**
 * @brief 获取屏幕 UI 互斥锁。
 *
 * app/UI 线程在访问 LVGL 对象树前应先加锁，访问完成后调用
 * service_screen_unlock() 释放。
 *
 * @param[in] timeout_ms 等待超时时间，0 表示一直等待。
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_lock(uint32_t timeout_ms);

/**
 * @brief 释放屏幕 UI 互斥锁。
 */
void service_screen_unlock(void);

/**
 * @brief 获取屏幕水平分辨率。
 *
 * @return 水平分辨率。
 */
uint16_t service_screen_get_hres(void);

/**
 * @brief 获取屏幕垂直分辨率。
 *
 * @return 垂直分辨率。
 */
uint16_t service_screen_get_vres(void);

/**
 * @brief 获取 LVGL 显示对象。
 *
 * @return 已初始化时返回显示对象，否则返回 NULL。
 */
lv_display_t *service_screen_get_display(void);

/**
 * @brief 获取 LVGL 触摸输入对象。
 *
 * @return 已初始化时返回输入对象，否则返回 NULL。
 */
lv_indev_t *service_screen_get_touch_indev(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_SCREEN_H */
