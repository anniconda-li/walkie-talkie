/**
 * @file service_camera.h
 * @brief 摄像头采集能力服务接口。
 *
 * service_camera 只抽象摄像头采集能力：输出模式切换、获取帧、归还帧。
 * 本头文件不暴露 ESP camera、LCD、LVGL 或具体 driver 类型。App 通过
 * service_camera 获取图像数据，再组合 service_screen 或 service_network
 * 完成预览、拍照上传等业务。
 */
#ifndef SERVICE_CAMERA_H
#define SERVICE_CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 摄像头帧数据格式。
 */
typedef enum {
    SERVICE_CAMERA_FORMAT_UNKNOWN = 0, /**< 未知或未识别格式。 */
    SERVICE_CAMERA_FORMAT_RGB565,      /**< RGB565 原始像素，通常用于本地预览。 */
    SERVICE_CAMERA_FORMAT_JPEG,        /**< JPEG 压缩图像，通常用于拍照上传。 */
} service_camera_format_t;

/**
 * @brief 通用摄像头帧视图。
 *
 * 此结构只描述一帧图像数据，不拥有底层帧缓存。通过 service_camera_get_frame()
 * 获取后，调用方必须尽快调用 service_camera_return_frame() 归还。
 *
 * opaque 是底层帧缓存归还凭据，只允许 service_camera_return_frame() 使用。
 * App 不应读取、修改或保存 opaque。
 */
typedef struct {
    const uint8_t *data;              /**< 帧数据起始地址。 */
    uint32_t len;                     /**< 帧数据长度，单位字节。 */
    uint16_t width;                   /**< 图像宽度，单位像素。 */
    uint16_t height;                  /**< 图像高度，单位像素。 */
    service_camera_format_t format;   /**< 帧数据格式。 */
    void *opaque;                     /**< 底层帧缓存归还凭据，调用方不可解析。 */
} service_camera_frame_t;

/**
 * @brief 摄像头服务依赖的下层采集能力。
 */
typedef struct {
    int (*is_initialized)(void);                  /**< 判断下层摄像头 driver 是否已初始化。 */
    int (*set_rgb565_mode)(void);                 /**< 切换到 RGB565 连续取帧模式。 */
    int (*set_jpeg_mode)(void);                   /**< 切换到 JPEG 拍照模式。 */
    int (*get_frame)(service_camera_frame_t *frame); /**< 获取当前模式下的一帧图像。 */
    void (*return_frame)(void *opaque);           /**< 归还底层帧缓存。 */
} service_camera_ops_t;

/**
 * @brief 摄像头服务初始化配置。
 */
typedef struct {
    service_camera_ops_t ops; /**< 下层摄像头能力函数表。 */
} service_camera_config_t;

/**
 * @brief 初始化摄像头服务。
 *
 * 初始化时复制 cfg 中的 ops，并严格检查下层 camera driver 已经初始化。
 * service 后续只调用自己保存的函数表，不直接依赖具体摄像头实现。
 *
 * @param[in] cfg 摄像头 service 初始化配置。
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_init(const service_camera_config_t *cfg);

/**
 * @brief 释放摄像头服务。
 *
 * 仅清空 service 内部能力函数表，不释放底层摄像头硬件。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_deinit(void);

/**
 * @brief 判断摄像头服务是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int service_camera_is_initialized(void);

/**
 * @brief 切换到 RGB565 连续取帧模式。
 *
 * 该模式用于本地预览。函数只切换摄像头输出格式，不负责把图像显示到屏幕。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_set_rgb565_mode(void);

/**
 * @brief 切换到 JPEG 拍照模式。
 *
 * 该模式用于获取 JPEG 压缩图像。切换后建议调用 service_camera_discard_frames()
 * 丢弃 1-2 帧，等待传感器输出稳定。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_set_jpeg_mode(void);

/**
 * @brief 获取当前模式下的一帧图像。
 *
 * 成功返回的 frame 只是底层帧缓存视图，调用方使用完必须调用
 * service_camera_return_frame()。如果需要长期保存 JPEG，应在 app 层拷贝到
 * 自己持有的缓冲区后立刻归还 frame。
 *
 * @param[out] frame 帧视图输出。
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_get_frame(service_camera_frame_t *frame);

/**
 * @brief 归还通过 service_camera_get_frame() 获取的帧。
 *
 * 归还后 frame 中的 data/opaque 不再有效，函数会清空 frame 内容，避免调用方
 * 继续误用已归还的底层帧缓存。
 *
 * @param[in,out] frame 待归还的帧视图。
 */
void service_camera_return_frame(service_camera_frame_t *frame);

/**
 * @brief 丢弃若干帧。
 *
 * 模式切换后传感器前几帧可能仍是旧格式或曝光未稳定。此接口循环取帧并立即
 * 归还，用于稳定 RGB565/JPEG 切换后的输出。
 *
 * @param[in] count 需要丢弃的帧数。
 * @return 成功返回 0；失败返回负值。
 */
int service_camera_discard_frames(uint8_t count);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_CAMERA_H */
