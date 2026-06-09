/**
 * @file d_camera.h
 * @brief WDRIVER 摄像头驱动接口。
 *
 * 本驱动基于 espressif/esp32-camera 组件封装摄像头初始化、取帧和释放。
 * 摄像头 SCCB 使用板级 I2C 引脚，初始化参数按 esp-camera 的 camera_config_t
 * 填充；摄像头层不释放 WDRIVER I2C，避免影响同总线的 PCA9557 和 LCD touch。
 */
#ifndef D_CAMERA_H
#define D_CAMERA_H

#include "d_config.h"
#include "wdriver_config.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define d_camera_VSYNC_IO WDRIVER_CAMERA_VSYNC_IO
#define d_camera_HREF_IO WDRIVER_CAMERA_HREF_IO
#define d_camera_PCLK_IO WDRIVER_CAMERA_PCLK_IO
#define d_camera_D0_IO WDRIVER_CAMERA_D0_IO
#define d_camera_D1_IO WDRIVER_CAMERA_D1_IO
#define d_camera_D2_IO WDRIVER_CAMERA_D2_IO
#define d_camera_D3_IO WDRIVER_CAMERA_D3_IO
#define d_camera_D4_IO WDRIVER_CAMERA_D4_IO
#define d_camera_D5_IO WDRIVER_CAMERA_D5_IO
#define d_camera_D6_IO WDRIVER_CAMERA_D6_IO
#define d_camera_D7_IO WDRIVER_CAMERA_D7_IO
#define d_camera_SIOD_IO WDRIVER_CAMERA_SIOD_IO
#define d_camera_SIOC_IO WDRIVER_CAMERA_SIOC_IO
#define d_camera_PWDN_IO WDRIVER_CAMERA_PWDN_IO
#define d_camera_RESET_IO WDRIVER_CAMERA_RESET_IO
#define d_camera_XCLK_IO WDRIVER_CAMERA_XCLK_IO

/** @brief 摄像头 XCLK 频率。 */
#define d_camera_XCLK_FREQ_HZ 20000000

/** @brief 本地预览使用的帧尺寸。 */
#define D_CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_240X240

/** @brief 拍照上传使用的 JPEG 帧尺寸。 */
#define D_CAMERA_CAPTURE_FRAME_SIZE FRAMESIZE_VGA

/**
 * @brief 初始化摄像头。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_camera_init(void);

/**
 * @brief 释放摄像头驱动。
 *
 * @return 成功返回 0；失败返回负值。
 * @note 本函数不释放 WDRIVER I2C 总线，避免影响同总线的 PCA9557 和 LCD touch。
 */
int d_camera_deinit(void);

/**
 * @brief 切换摄像头到 RGB565 预览模式。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_camera_set_rgb565_mode(void);

/**
 * @brief 切换摄像头到 JPEG 拍照模式。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_camera_set_jpeg_mode(void);

/**
 * @brief 判断摄像头驱动是否已经初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int d_camera_is_initialized(void);

/**
 * @brief 获取一帧摄像头图像。
 *
 * @return 成功返回帧缓存指针；失败返回 NULL。
 * @note 使用完成后必须调用 d_camera_return_frame() 归还帧缓存。
 */
camera_fb_t *d_camera_get_frame(void);

/**
 * @brief 归还摄像头帧缓存。
 *
 * @param[in] fb 待归还的帧缓存指针。
 */
void d_camera_return_frame(camera_fb_t *fb);

/**
 * @brief 获取摄像头传感器控制对象。
 *
 * @return 成功返回 sensor 指针；失败返回 NULL。
 */
sensor_t *d_camera_get_sensor(void);

#ifdef __cplusplus
}
#endif

#endif /* D_CAMERA_H */
