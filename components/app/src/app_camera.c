/**
 * @file app_camera.c
 * @brief 相机业务模块实现。
 *
 * 分层边界：
 * - service_camera 只提供取帧和模式切换；
 * - service_screen 只提供 RGB565 直绘能力；
 * - service_network 只提供 HTTP POST；
 * - app_camera 负责把三者组合成“预览、拍照、上传、重拍”的业务流程。
 */
#include "app_camera.h"

#include "app_config.h"
#include "osal_heap.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_camera.h"
#include "service_network.h"
#include "service_screen.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "app_camera";

/** @brief 相机后台任务栈大小。 */
#define APP_CAMERA_TASK_STACK          4096u
/** @brief 相机后台任务优先级，低于 PTT，避免影响实时音频。 */
#define APP_CAMERA_TASK_PRIORITY       4u
/** @brief 模式切换后丢弃帧数，用于等待 sensor 输出稳定。 */
#define APP_CAMERA_MODE_DISCARD_FRAMES 2u

/** @brief 相机后台任务句柄。 */
static osal_task_t s_camera_task = NULL;
/** @brief 相机业务状态锁，保护请求标志和 JPEG 暂存状态。 */
static osal_mutex_t s_camera_mutex = NULL;
/** @brief 相机业务是否已启动。 */
static volatile int s_started = 0;
/** @brief 当前相机页是否处于进入状态。 */
static volatile int s_page_active = 0;
/** @brief 当前是否允许预览任务持续刷新。 */
static volatile int s_preview_active = 0;
/** @brief 当前是否已拍照定格。 */
static volatile int s_frozen = 0;
/** @brief 拍照请求标志，由 UI 回调置位，后台任务消费。 */
static volatile int s_capture_req = 0;
/** @brief 上传请求标志，由 UI 回调置位，后台任务消费。 */
static volatile int s_upload_req = 0;
/** @brief 重拍请求标志，由 UI 回调置位，后台任务消费。 */
static volatile int s_retake_req = 0;
/** @brief JPEG 暂存缓冲，分配在外部大容量内存。 */
static uint8_t *s_jpeg_buf = NULL;
/** @brief JPEG 暂存缓冲容量。 */
static uint32_t s_jpeg_buf_size = 0u;
/** @brief 当前暂存 JPEG 有效长度。 */
static uint32_t s_jpeg_len = 0u;
/** @brief HTTP 上传响应临时缓冲。 */
static uint8_t s_upload_resp[APP_CAMERA_UPLOAD_RESP_BYTES];

/**
 * @brief 锁定相机业务状态。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int app_camera_lock(void)
{
    if (s_camera_mutex == NULL) {
        return -1;
    }

    return osal_mutex_lock(s_camera_mutex, OSAL_WAIT_FOREVER);
}

/** @brief 解锁相机业务状态。 */
static void app_camera_unlock(void)
{
    if (s_camera_mutex != NULL) {
        osal_mutex_unlock(s_camera_mutex);
    }
}

/** @brief 唤醒相机后台任务处理 UI 请求。 */
static void app_camera_notify_task(void)
{
    if (s_camera_task != NULL) {
        (void)osal_task_notify_give(s_camera_task);
    }
}

/**
 * @brief 释放 app 层暂存的 JPEG 数据。
 *
 * JPEG 是拍照业务产物，由 app_camera 持有。摄像头底层 frame 在拍照时已经
 * 归还给 service_camera，上传和重拍只操作这里的业务缓存。
 */
static void app_camera_clear_jpeg(void)
{
    if (s_jpeg_buf != NULL) {
        osal_heap_free(s_jpeg_buf);
        s_jpeg_buf = NULL;
    }
    s_jpeg_buf_size = 0u;
    s_jpeg_len = 0u;
}

/**
 * @brief 确保 JPEG 暂存缓冲容量足够。
 *
 * @param[in] len 需要保存的 JPEG 字节数。
 * @return 成功返回 0；失败返回负值。
 */
static int app_camera_ensure_jpeg_buffer(uint32_t len)
{
    if (len == 0u) {
        return -1;
    }
    if (s_jpeg_buf != NULL && s_jpeg_buf_size >= len) {
        return 0;
    }

    app_camera_clear_jpeg();
    s_jpeg_buf = (uint8_t *)osal_heap_alloc_external(len);
    if (s_jpeg_buf == NULL) {
        APP_LOGE(TAG,
                 "相机 JPEG 缓冲分配失败, bytes=%u, external_free=%u",
                 (unsigned int)len,
                 (unsigned int)osal_heap_get_external_free_size());
        return -2;
    }

    s_jpeg_buf_size = len;
    return 0;
}

/**
 * @brief 处理一帧 RGB565 预览。
 *
 * 获取 camera frame 后立即通过 service_screen 直绘，然后归还 frame。这里不
 * 保存预览帧，因此拍照定格依赖 LCD 上最后一次直绘结果。
 */
static void app_camera_preview_once(void)
{
    service_camera_frame_t frame;
    int ret = service_camera_get_frame(&frame);
    if (ret != 0) {
        osal_delay_ms(APP_CAMERA_PREVIEW_INTERVAL_MS);
        return;
    }

    if (frame.format == SERVICE_CAMERA_FORMAT_RGB565 &&
        frame.width == APP_CAMERA_PREVIEW_W &&
        frame.height == APP_CAMERA_PREVIEW_H &&
        frame.len >= (APP_CAMERA_PREVIEW_W * APP_CAMERA_PREVIEW_H * sizeof(uint16_t))) {
        (void)service_screen_draw_rgb565(APP_CAMERA_PREVIEW_X,
                                         APP_CAMERA_PREVIEW_Y,
                                         APP_CAMERA_PREVIEW_W,
                                         APP_CAMERA_PREVIEW_H,
                                         frame.data);
    }

    service_camera_return_frame(&frame);
    osal_delay_ms(APP_CAMERA_PREVIEW_INTERVAL_MS);
}

/**
 * @brief 执行拍照流程。
 *
 * 先停止预览，让 LCD 保留最后一帧 RGB565 画面；再切到 JPEG 模式取一帧，
 * 拷贝到 app 层 PSRAM 缓冲后立即归还底层 frame。
 */
static void app_camera_do_capture(void)
{
    if (service_camera_is_initialized() != 1) {
        return;
    }

    s_preview_active = 0;
    s_frozen = 1;

    int ret = service_camera_set_jpeg_mode();
    if (ret == 0) {
        ret = service_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
    }
    if (ret != 0) {
        APP_LOGW(TAG, "相机切换 JPEG 模式失败, ret=%d", ret);
        return;
    }

    service_camera_frame_t frame;
    ret = service_camera_get_frame(&frame);
    if (ret != 0) {
        APP_LOGW(TAG, "相机 JPEG 取帧失败, ret=%d", ret);
        return;
    }

    if (frame.format != SERVICE_CAMERA_FORMAT_JPEG || frame.data == NULL || frame.len == 0u) {
        APP_LOGW(TAG,
                 "相机 JPEG 帧无效, format=%d, data=%p, len=%u",
                 (int)frame.format,
                 frame.data,
                 (unsigned int)frame.len);
        service_camera_return_frame(&frame);
        return;
    }

    ret = app_camera_ensure_jpeg_buffer(frame.len);
    if (ret == 0) {
        memcpy(s_jpeg_buf, frame.data, frame.len);
        s_jpeg_len = frame.len;
        APP_LOGI(TAG,
                 "相机拍照完成, jpeg_len=%u, size=%ux%u",
                 (unsigned int)s_jpeg_len,
                 (unsigned int)frame.width,
                 (unsigned int)frame.height);
    }

    service_camera_return_frame(&frame);
}

/**
 * @brief 执行 JPEG 上传。
 */
static void app_camera_do_upload(void)
{
    if (s_jpeg_buf == NULL || s_jpeg_len == 0u) {
        APP_LOGW(TAG, "相机上传失败: 没有可上传的 JPEG");
        return;
    }

    uint32_t resp_len = 0u;
    int ret = service_network_http_post(APP_BUSINESS_CAMERA_UPLOAD_URL,
                                        "image/jpeg",
                                        s_jpeg_buf,
                                        s_jpeg_len,
                                        s_upload_resp,
                                        sizeof(s_upload_resp) - 1u,
                                        &resp_len,
                                        APP_CAMERA_UPLOAD_TIMEOUT_MS);
    if (ret == 0) {
        s_upload_resp[resp_len < sizeof(s_upload_resp) ? resp_len : (sizeof(s_upload_resp) - 1u)] = '\0';
        APP_LOGI(TAG, "相机 JPEG 上传成功, len=%u, resp_len=%u",
                 (unsigned int)s_jpeg_len,
                 (unsigned int)resp_len);
    } else {
        APP_LOGW(TAG, "相机 JPEG 上传失败, ret=%d, len=%u",
                 ret,
                 (unsigned int)s_jpeg_len);
    }
}

/**
 * @brief 执行重拍流程。
 *
 * 释放旧 JPEG，切回 RGB565 并恢复预览。
 */
static void app_camera_do_retake(void)
{
    app_camera_clear_jpeg();
    s_frozen = 0;
    if (s_page_active != 0 && service_camera_set_rgb565_mode() == 0) {
        (void)service_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
        s_preview_active = 1;
    }
}

/**
 * @brief 消费 UI 请求标志。
 *
 * 返回前把标志从全局状态中取出并清零，保证一次 UI 操作只处理一次。
 */
static void app_camera_take_requests(int *capture, int *upload, int *retake)
{
    *capture = 0;
    *upload = 0;
    *retake = 0;

    if (app_camera_lock() != 0) {
        return;
    }
    *capture = s_capture_req;
    *upload = s_upload_req;
    *retake = s_retake_req;
    s_capture_req = 0;
    s_upload_req = 0;
    s_retake_req = 0;
    app_camera_unlock();
}

/**
 * @brief 相机后台任务入口。
 *
 * 任务在相机页激活时持续做低频 RGB565 预览；拍照、上传、重拍通过 UI 回调
 * 置位请求标志并 notify 任务，由这里串行处理，避免在 LVGL 线程做阻塞工作。
 */
static void app_camera_task(void *arg)
{
    (void)arg;

    while (1) {
        int capture = 0;
        int upload = 0;
        int retake = 0;
        app_camera_take_requests(&capture, &upload, &retake);

        if (retake) {
            app_camera_do_retake();
        }
        if (capture) {
            app_camera_do_capture();
        }
        if (upload) {
            app_camera_do_upload();
        }

        if (s_preview_active != 0 && s_frozen == 0 && s_page_active != 0) {
            app_camera_preview_once();
        } else {
            (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        }
    }
}

int app_camera_start(void)
{
    if (s_started) {
        return 0;
    }

    if (service_camera_is_initialized() != 1) {
        APP_LOGW(TAG, "相机服务未初始化，相机业务保持禁用");
        s_started = 1;
        return 0;
    }

    s_camera_mutex = osal_mutex_create();
    if (s_camera_mutex == NULL) {
        APP_LOGE(TAG, "相机业务互斥锁创建失败");
        return -1;
    }

    int ret = osal_task_create("biz_camera",
                               app_camera_task,
                               NULL,
                               APP_CAMERA_TASK_STACK,
                               APP_CAMERA_TASK_PRIORITY,
                               &s_camera_task);
    if (ret != 0) {
        APP_LOGE(TAG, "相机业务任务创建失败, ret=%d", ret);
        return ret;
    }

    s_started = 1;
    APP_LOGI(TAG, "相机业务启动完成");
    return 0;
}

void app_camera_enter(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return;
    }

    s_page_active = 1;
    s_frozen = 0;
    /*
     * 模式切换和丢帧可能阻塞几十毫秒，不能放在 LVGL 事件回调线程里执行。
     * 这里复用重拍请求，让后台任务清理旧 JPEG 并切回 RGB565 预览模式。
     */
    if (app_camera_lock() == 0) {
        s_retake_req = 1;
        app_camera_unlock();
    }
    app_camera_notify_task();
}

void app_camera_exit(void)
{
    if (!s_started) {
        return;
    }

    s_page_active = 0;
    s_preview_active = 0;
    s_frozen = 0;
    if (app_camera_lock() == 0) {
        s_capture_req = 0;
        s_upload_req = 0;
        /*
         * 退出页面时让后台任务负责释放 JPEG。这样即使当前正在上传，也不会
         * 在 UI 线程里等待 HTTP，也不会边上传边释放缓冲。
         */
        s_retake_req = 1;
        app_camera_unlock();
    }
    app_camera_notify_task();
}

void app_camera_capture(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return;
    }

    if (app_camera_lock() == 0) {
        s_capture_req = 1;
        app_camera_unlock();
    }
    app_camera_notify_task();
}

void app_camera_upload(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return;
    }

    if (app_camera_lock() == 0) {
        s_upload_req = 1;
        app_camera_unlock();
    }
    app_camera_notify_task();
}

void app_camera_retake(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return;
    }

    if (app_camera_lock() == 0) {
        s_retake_req = 1;
        app_camera_unlock();
    }
    app_camera_notify_task();
}
