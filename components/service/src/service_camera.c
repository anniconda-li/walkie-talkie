/**
 * @file service_camera.c
 * @brief 摄像头采集能力服务实现。
 *
 * 本文件只处理摄像头能力抽象，不处理 LCD 预览显示、不处理 JPEG 上传、
 * 不包含 ESP camera 或具体 driver 头文件。具体帧类型转换由 service_init
 * 装配层完成，service_camera 只保存并调用自己的 ops。
 */
#include "service_camera.h"

#include "service_config.h"

#include <stddef.h>
#include <string.h>

/** @brief 摄像头服务日志标签。 */
static const char *TAG = "service_camera";

/** @brief 当前绑定的摄像头 driver 能力函数表。 */
static service_camera_ops_t s_camera_ops;

/** @brief 摄像头 ops 是否已经完成绑定并通过初始化检查。 */
static uint8_t s_camera_ops_ready = 0u;

/**
 * @brief 检查摄像头 service 运行所需能力是否完整。
 *
 * service 只接受自己定义的通用能力，不关心下层是 OV5640、OV2640 还是
 * 其他传感器。任一函数为空都会导致初始化失败，避免运行时空指针调用。
 *
 * @param[in] ops 待检查的能力函数表。
 * @return 有效返回 0；无效返回 -1。
 */
static int service_camera_ops_is_valid(const service_camera_ops_t *ops)
{
    if (ops == NULL ||
        ops->is_initialized == NULL ||
        ops->set_rgb565_mode == NULL ||
        ops->set_jpeg_mode == NULL ||
        ops->get_frame == NULL ||
        ops->return_frame == NULL) {
        return -1;
    }

    return 0;
}

int service_camera_init(const service_camera_config_t *cfg)
{
    if (s_camera_ops_ready != 0u) {
        SERVICE_LOGI(TAG, "摄像头服务已初始化");
        return 0;
    }

    if (cfg == NULL || service_camera_ops_is_valid(&cfg->ops) != 0) {
        SERVICE_LOGE(TAG, "摄像头服务初始化失败: ops 无效");
        return -1;
    }
    if (cfg->ops.is_initialized() != 1) {
        SERVICE_LOGE(TAG, "摄像头服务初始化失败: 下层摄像头 driver 未初始化");
        return -2;
    }

    s_camera_ops = cfg->ops;
    s_camera_ops_ready = 1u;
    SERVICE_LOGI(TAG, "摄像头服务初始化成功");
    return 0;
}

int service_camera_deinit(void)
{
    s_camera_ops = (service_camera_ops_t){0};
    s_camera_ops_ready = 0u;
    SERVICE_LOGI(TAG, "摄像头服务已释放");
    return 0;
}

int service_camera_is_initialized(void)
{
    return s_camera_ops_ready != 0u ? 1 : 0;
}

int service_camera_set_rgb565_mode(void)
{
    /*
     * RGB565 模式只表示摄像头输出连续预览帧。是否显示、显示到哪里，由
     * app_camera 组合 service_screen 来决定，避免 camera service 越界。
     */
    if (s_camera_ops_ready == 0u) {
        return -1;
    }

    return s_camera_ops.set_rgb565_mode();
}

int service_camera_set_jpeg_mode(void)
{
    /*
     * JPEG 模式用于拍照上传。service 不保存 JPEG，也不分配业务缓存；
     * 调用方应在取到 JPEG frame 后拷贝到自己的缓冲区并立即归还。
     */
    if (s_camera_ops_ready == 0u) {
        return -1;
    }

    return s_camera_ops.set_jpeg_mode();
}

int service_camera_get_frame(service_camera_frame_t *frame)
{
    if (s_camera_ops_ready == 0u || frame == NULL) {
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    return s_camera_ops.get_frame(frame);
}

void service_camera_return_frame(service_camera_frame_t *frame)
{
    if (s_camera_ops_ready == 0u || frame == NULL || frame->opaque == NULL) {
        return;
    }

    /*
     * opaque 是装配层放入的底层帧缓存凭据。service 不解析它，只原样交回
     * return_frame()。清空 frame 可以减少上层误用已归还数据的概率。
     */
    s_camera_ops.return_frame(frame->opaque);
    memset(frame, 0, sizeof(*frame));
}

int service_camera_discard_frames(uint8_t count)
{
    if (s_camera_ops_ready == 0u) {
        return -1;
    }

    for (uint8_t i = 0; i < count; i++) {
        service_camera_frame_t frame;
        int ret = service_camera_get_frame(&frame);
        if (ret != 0) {
            SERVICE_LOGW(TAG, "摄像头丢帧失败, index=%u, ret=%d",
                         (unsigned int)i,
                         ret);
            return ret;
        }
        service_camera_return_frame(&frame);
    }

    return 0;
}
