/**
 * @file service_screen.h
 * @brief 屏幕服务接口。
 *
 * 本服务组合 LCD 显示、触摸和 LVGL 运行环境，对 app 层提供屏幕 UI 运行能力。
 */
#ifndef SERVICE_SCREEN_H
#define SERVICE_SCREEN_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化屏幕服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_init(void);

/**
 * @brief 释放屏幕服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_deinit(void);

/**
 * @brief 获取屏幕 UI 互斥锁。
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
