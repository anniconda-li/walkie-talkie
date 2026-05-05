/**
 * @file service_lvgl.h
 * @brief LVGL 运行时服务接口。
 *
 * 本服务只提供 LVGL 初始化、锁和显示输入设备访问能力，不负责 UI 页面设计。
 */
#ifndef SERVICE_LVGL_H
#define SERVICE_LVGL_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 运行环境。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_lvgl_init(void);

/**
 * @brief 释放 LVGL 运行环境。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_lvgl_deinit(void);

/**
 * @brief 获取 LVGL 互斥锁。
 *
 * @param[in] timeout_ms 等待超时时间，0 表示一直等待。
 * @return 成功返回 0；失败返回负值。
 */
int service_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 互斥锁。
 */
void service_lvgl_unlock(void);

/**
 * @brief 获取 LVGL 水平分辨率。
 *
 * @return 水平分辨率。
 */
uint16_t service_lvgl_get_hres(void);

/**
 * @brief 获取 LVGL 垂直分辨率。
 *
 * @return 垂直分辨率。
 */
uint16_t service_lvgl_get_vres(void);

/**
 * @brief 获取 LVGL 显示对象。
 *
 * @return 已初始化时返回显示对象，否则返回 NULL。
 */
lv_display_t *service_lvgl_get_display(void);

/**
 * @brief 获取 LVGL 触摸输入对象。
 *
 * @return 已初始化时返回输入对象，否则返回 NULL。
 */
lv_indev_t *service_lvgl_get_touch_indev(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_LVGL_H */
