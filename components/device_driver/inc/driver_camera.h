/**
 * @file driver_camera.h
 * @brief BSP 摄像头驱动接口。
 *
 * 本驱动基于 espressif/esp32-camera 组件封装摄像头初始化、取帧和释放。
 * 摄像头 SCCB 复用 BSP I2C 已初始化的新 I2C 总线，不在摄像头层创建或释放 I2C。
 */
#ifndef driver_camera_H
#define driver_camera_H

#include "driver_config.h"
#include "bsp_config.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define driver_camera_VSYNC_IO BSP_CAMERA_VSYNC_IO
#define driver_camera_HREF_IO BSP_CAMERA_HREF_IO
#define driver_camera_PCLK_IO BSP_CAMERA_PCLK_IO
#define driver_camera_D0_IO BSP_CAMERA_D0_IO
#define driver_camera_D1_IO BSP_CAMERA_D1_IO
#define driver_camera_D2_IO BSP_CAMERA_D2_IO
#define driver_camera_D3_IO BSP_CAMERA_D3_IO
#define driver_camera_D4_IO BSP_CAMERA_D4_IO
#define driver_camera_D5_IO BSP_CAMERA_D5_IO
#define driver_camera_D6_IO BSP_CAMERA_D6_IO
#define driver_camera_D7_IO BSP_CAMERA_D7_IO
#define driver_camera_PWDN_IO BSP_CAMERA_PWDN_IO
#define driver_camera_RESET_IO BSP_CAMERA_RESET_IO
#define driver_camera_XCLK_IO BSP_CAMERA_XCLK_IO

/**
 * @brief 初始化摄像头。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_camera_init(void);

/**
 * @brief 释放摄像头驱动。
 *
 * @return 成功返回 0；失败返回负值。
 * @note 本函数不释放 BSP I2C 总线，避免影响同总线的 PCA9557 和 LCD touch。
 */
int driver_camera_deinit(void);

/**
 * @brief 切换摄像头到 RGB565 预览模式。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_camera_set_rgb565_mode(void);

/**
 * @brief 切换摄像头到 JPEG 拍照模式。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_camera_set_jpeg_mode(void);

/**
 * @brief 判断摄像头驱动是否已经初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int driver_camera_is_initialized(void);

/**
 * @brief 获取一帧摄像头图像。
 *
 * @return 成功返回帧缓存指针；失败返回 NULL。
 * @note 使用完成后必须调用 driver_camera_return_frame() 归还帧缓存。
 */
camera_fb_t *driver_camera_get_frame(void);

/**
 * @brief 归还摄像头帧缓存。
 *
 * @param[in] fb 待归还的帧缓存指针。
 */
void driver_camera_return_frame(camera_fb_t *fb);

/**
 * @brief 获取摄像头传感器控制对象。
 *
 * @return 成功返回 sensor 指针；失败返回 NULL。
 */
sensor_t *driver_camera_get_sensor(void);

#ifdef __cplusplus
}
#endif

#endif /* driver_camera_H */
