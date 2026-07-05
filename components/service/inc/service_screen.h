/**
 * @file service_screen.h
 * @brief 屏幕服务接口。
 *
 * 本服务组合 LCD 显示、触摸和 LVGL 运行环境，对 app 层提供屏幕 UI 运行能力。
 */
#ifndef SERVICE_SCREEN_H
#define SERVICE_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 屏幕服务依赖的下层显示与触摸能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层屏幕 driver 是否已初始化。 */
    void *(*get_panel_io)(void); /**< 获取下层显示 IO 不透明句柄，仅供 service 内部接入 UI runtime。 */
    void *(*get_panel)(void);    /**< 获取下层显示 panel 不透明句柄，仅供 service 内部接入 UI runtime。 */
    void *(*get_touch)(void);    /**< 获取下层触摸不透明句柄，仅供 service 内部接入 UI runtime。 */
    int (*display_on)(int on);    /**< 设置下层显示和背光开关。 */
    int (*set_brightness)(uint8_t percent); /**< 设置下层背光亮度百分比。 */
    int (*draw_rgb565)(int x,
                       int y,
                       int w,
                       int h,
                       const void *data); /**< 绘制 RGB565 位图到屏幕指定区域。 */
    uint16_t hres;          /**< 水平分辨率。 */
    uint16_t vres;          /**< 垂直分辨率。 */
    uint8_t swap_xy;        /**< 显示方向 swap_xy 配置。 */
    uint8_t mirror_x;       /**< 显示方向 mirror_x 配置。 */
    uint8_t mirror_y;       /**< 显示方向 mirror_y 配置。 */
} service_screen_device_ops_t;

/**
 * @brief 初始化屏幕服务。
 *
 * 初始化时会复制下层屏幕能力函数表，并把 driver 已创建的 LCD panel/touch
 * 句柄接入 LVGL port。service 不负责初始化具体 LCD 或触摸芯片。
 *
 * @param[in] ops 下层显示与触摸能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_init(const service_screen_device_ops_t *ops);

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
 * @brief 判断屏幕服务是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int service_screen_is_initialized(void);

/**
 * @brief 绘制 RGB565 位图到屏幕指定区域。
 *
 * 本接口用于摄像头预览等高频原始图像显示场景。App 层只调用 screen service，
 * 不直接访问 LCD driver 或 ESP LCD panel。函数内部会完成屏幕范围检查，并
 * 通过 LVGL port 锁保护 LCD 访问，避免与普通 UI flush 并发。
 *
 * @param[in] x 起始 X 坐标。
 * @param[in] y 起始 Y 坐标。
 * @param[in] w 绘制宽度，单位像素。
 * @param[in] h 绘制高度，单位像素。
 * @param[in] data RGB565 原始像素数据，长度至少为 w * h * 2 字节。
 * @return 成功返回 0；失败返回负值。
 *
 * @note 调用方不应在已经持有 service_screen_lock() 的情况下再调用本接口，
 *       避免重复加锁造成阻塞。
 */
int service_screen_draw_rgb565(int x, int y, int w, int h, const void *data);

/**
 * @brief 设置屏幕显示和背光开关。
 *
 * @param[in] on 0 关闭显示，非 0 打开显示。
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_display_on(int on);

/**
 * @brief 查询当前屏幕是否支持亮度百分比调节。
 *
 * @return 支持返回 1；不支持或未初始化返回 0。
 */
int service_screen_supports_brightness(void);

/**
 * @brief 设置屏幕背光亮度。
 *
 * @param[in] percent 亮度百分比，范围 0-100。
 * @return 成功返回 0；失败返回负值。
 */
int service_screen_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_SCREEN_H */
