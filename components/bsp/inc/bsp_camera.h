/**
 * @file bsp_camera.h
 * @brief BSP 摄像头驱动接口。
 *
 * 本驱动基于 espressif/esp32-camera 组件封装摄像头初始化、取帧和释放。
 * 摄像头 SCCB 复用 BSP I2C 已初始化的新 I2C 总线，不在摄像头层创建或释放 I2C。
 */
#ifndef BSP_CAMERA_H
#define BSP_CAMERA_H

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化摄像头。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_camera_init(void);

/**
 * @brief 释放摄像头驱动。
 *
 * @return 成功返回 0；失败返回负值。
 * @note 本函数不释放 BSP I2C 总线，避免影响同总线的 PCA9557 和 LCD touch。
 */
int bsp_camera_deinit(void);

/**
 * @brief 获取一帧摄像头图像。
 *
 * @return 成功返回帧缓存指针；失败返回 NULL。
 * @note 使用完成后必须调用 bsp_camera_return_frame() 归还帧缓存。
 */
camera_fb_t *bsp_camera_get_frame(void);

/**
 * @brief 归还摄像头帧缓存。
 *
 * @param[in] fb 待归还的帧缓存指针。
 */
void bsp_camera_return_frame(camera_fb_t *fb);

/**
 * @brief 获取摄像头传感器控制对象。
 *
 * @return 成功返回 sensor 指针；失败返回 NULL。
 */
sensor_t *bsp_camera_get_sensor(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_CAMERA_H */
