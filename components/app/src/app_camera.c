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

#include "app_ui.h"
#include "app_config.h"
#include "osal_heap.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_camera.h"
#include "service_network.h"
#include "service_screen.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_camera";

/** @brief 相机后台任务栈大小。 */
#define APP_CAMERA_TASK_STACK          4096u
/** @brief 相机后台任务优先级，低于 PTT，避免影响实时音频。 */
#define APP_CAMERA_TASK_PRIORITY       4u
/** @brief 模式切换后丢弃帧数，用于等待 sensor 输出稳定。 */
#define APP_CAMERA_MODE_DISCARD_FRAMES 5u
/** @brief JPEG 模式切换后最多尝试取帧次数。 */
#define APP_CAMERA_JPEG_CAPTURE_TRIES 10u

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
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_COLOR
/** @brief 固定色块测试缓冲，保存在 PSRAM，避免占用内部 SRAM。 */
static uint16_t *s_color_test_buf = NULL;
/** @brief 固定色块测试缓冲是否已经绘制过。 */
static uint8_t s_color_test_drawn = 0u;
#endif
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_SINGLE
/** @brief 单帧冻结测试是否已经绘制过。 */
static uint8_t s_single_test_drawn = 0u;
#endif

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
 * @brief 构造 FastAPI 业务路由 URL。
 */
static int app_camera_build_url(char *out, size_t out_size, const char *path)
{
    if (out == NULL || out_size == 0u || path == NULL) {
        return -1;
    }

    const char *base = APP_BUSINESS_HTTP_BASE_URL;
    size_t base_len = strlen(base);
    const char *path_start = path;
    while (*path_start == '/' && base_len > 0u && base[base_len - 1u] == '/') {
        path_start++;
    }

    int written = snprintf(out, out_size, "%s%s", base, path_start);
    return (written > 0 && (size_t)written < out_size) ? 0 : -2;
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
 * @brief 判断一帧数据是否具有完整 JPEG 头尾。
 *
 * esp-camera 在运行时从 RGB565 切到 JPEG 后，可能还会吐出旧缓冲帧。
 * 仅检查 frame.format 不够稳妥，这里同时检查 SOI/EOI，确认本地拿到的
 * 确实是可保存、可上传的 JPEG 码流。
 */
static int app_camera_frame_is_valid_jpeg(const service_camera_frame_t *frame)
{
    if (frame == NULL ||
        frame->format != SERVICE_CAMERA_FORMAT_JPEG ||
        frame->data == NULL ||
        frame->len < 4u) {
        return 0;
    }

    return (frame->data[0] == 0xFFu &&
            frame->data[1] == 0xD8u &&
            frame->data[frame->len - 2u] == 0xFFu &&
            frame->data[frame->len - 1u] == 0xD9u) ? 1 : 0;
}

/**
 * @brief 丢弃若干帧，等待摄像头模式切换后的输出稳定。
 *
 * RGB565/JPEG 模式切换后，底层可能还会返回旧模式缓冲或曝光未稳定的帧。
 * 这是相机业务自己的稳定策略，因此放在 app_camera 内部，而不是 service API。
 *
 * @param[in] count 需要丢弃的帧数。
 * @return 成功返回 0；取帧失败返回负值。
 */
static int app_camera_discard_frames(uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        service_camera_frame_t frame;
        int ret = service_camera_get_frame(&frame);
        if (ret != 0) {
            APP_LOGW(TAG, "相机丢帧失败, index=%u, ret=%d",
                     (unsigned int)i,
                     ret);
            return ret;
        }
        service_camera_return_frame(&frame);
    }

    return 0;
}

#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_COLOR
/**
 * @brief 绘制固定 RGB565 色块。
 *
 * 不经过 camera，只验证 service_screen_draw_rgb565()/LCD 直刷链路是否稳定。
 */
static void app_camera_preview_color_test_once(void)
{
    if (s_color_test_drawn != 0u) {
        s_preview_active = 0;
        s_frozen = 1;
        return;
    }

    const uint32_t pixels = APP_CAMERA_PREVIEW_W * APP_CAMERA_PREVIEW_H;
    if (s_color_test_buf == NULL) {
        s_color_test_buf = (uint16_t *)osal_heap_alloc_external(pixels * sizeof(uint16_t));
        if (s_color_test_buf == NULL) {
            APP_LOGE(TAG,
                     "相机固定色块测试缓冲分配失败, bytes=%u, external_free=%u",
                     (unsigned int)(pixels * sizeof(uint16_t)),
                     (unsigned int)osal_heap_get_external_free_size());
            s_preview_active = 0;
            return;
        }
    }

    for (uint32_t y = 0u; y < APP_CAMERA_PREVIEW_H; y++) {
        for (uint32_t x = 0u; x < APP_CAMERA_PREVIEW_W; x++) {
            uint16_t color = 0x07E0; /* green */
            if (x < (APP_CAMERA_PREVIEW_W / 3u)) {
                color = 0xF800;      /* red */
            } else if (x >= ((APP_CAMERA_PREVIEW_W * 2u) / 3u)) {
                color = 0x001F;      /* blue */
            }
            s_color_test_buf[(y * APP_CAMERA_PREVIEW_W) + x] = color;
        }
    }

    (void)service_screen_draw_rgb565(APP_CAMERA_PREVIEW_X,
                                     APP_CAMERA_PREVIEW_Y,
                                     APP_CAMERA_PREVIEW_W,
                                     APP_CAMERA_PREVIEW_H,
                                     s_color_test_buf);
    APP_LOGI(TAG, "相机固定色块测试已绘制");
    s_color_test_drawn = 1u;
    s_preview_active = 0;
    s_frozen = 1;
}
#endif

/**
 * @brief 处理一帧 RGB565 预览。
 *
 * 获取 camera frame 后立即通过 service_screen 直绘，然后归还 frame。这里不
 * 保存预览帧，因此拍照定格依赖 LCD 上最后一次直绘结果。
 */
static void app_camera_preview_once(void)
{
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_COLOR
    app_camera_preview_color_test_once();
    return;
#endif

#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_SINGLE
    if (s_single_test_drawn != 0u) {
        s_preview_active = 0;
        s_frozen = 1;
        return;
    }
#endif

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
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_SINGLE
        APP_LOGI(TAG,
                 "相机单帧冻结测试已绘制, format=%d, size=%ux%u, len=%u",
                 (int)frame.format,
                 (unsigned int)frame.width,
                 (unsigned int)frame.height,
                 (unsigned int)frame.len);
        s_single_test_drawn = 1u;
        s_preview_active = 0;
        s_frozen = 1;
#endif
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
        ret = app_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
    }
    if (ret != 0) {
        APP_LOGW(TAG, "相机切换 JPEG 模式失败, ret=%d", ret);
        return;
    }

    for (uint32_t i = 0u; i < APP_CAMERA_JPEG_CAPTURE_TRIES; i++) {
        service_camera_frame_t frame;
        ret = service_camera_get_frame(&frame);
        if (ret != 0) {
            APP_LOGW(TAG, "相机 JPEG 取帧失败, try=%u, ret=%d", (unsigned int)i, ret);
            return;
        }

        const uint8_t head0 = frame.len > 0u && frame.data != NULL ? frame.data[0] : 0u;
        const uint8_t head1 = frame.len > 1u && frame.data != NULL ? frame.data[1] : 0u;
        const uint8_t tail0 = frame.len > 1u && frame.data != NULL ? frame.data[frame.len - 2u] : 0u;
        const uint8_t tail1 = frame.len > 0u && frame.data != NULL ? frame.data[frame.len - 1u] : 0u;

        APP_LOGI(TAG,
                 "相机 JPEG 候选帧, try=%u, format=%d, size=%ux%u, len=%u, head=%02X%02X, tail=%02X%02X",
                 (unsigned int)i,
                 (int)frame.format,
                 (unsigned int)frame.width,
                 (unsigned int)frame.height,
                 (unsigned int)frame.len,
                 (unsigned int)head0,
                 (unsigned int)head1,
                 (unsigned int)tail0,
                 (unsigned int)tail1);

        if (app_camera_frame_is_valid_jpeg(&frame) == 0) {
            service_camera_return_frame(&frame);
            continue;
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
        return;
    }

    APP_LOGW(TAG, "相机 JPEG 取帧失败: 连续 %u 帧都不是合法 JPEG",
             (unsigned int)APP_CAMERA_JPEG_CAPTURE_TRIES);
}

/**
 * @brief 执行 JPEG 上传。
 */
static void app_camera_do_upload(void)
{
    int ret = 0;

    if (s_jpeg_buf == NULL || s_jpeg_len == 0u) {
        APP_LOGW(TAG, "相机上传失败: 没有可上传的 JPEG");
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        return;
    }

    if (service_network_is_ready() != 1) {
        APP_LOGW(TAG, "相机上传失败: 网络未就绪");
        app_camera_clear_jpeg();
        (void)app_ui_set_ai_message(UI_TEXT_AI_NO_NETWORK);
        return;
    }

    uint32_t resp_len = 0u;
    char url[256];
    ret = app_camera_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_CAMERA_UPLOAD);
    if (ret != 0) {
        APP_LOGW(TAG, "相机上传 URL 构造失败, ret=%d", ret);
        app_camera_clear_jpeg();
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        return;
    }

    ret = service_network_http_post(url,
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
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
    }

    app_camera_clear_jpeg();
    if (ret == 0) {
        (void)app_ui_set_ai_waiting(0);
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
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_SINGLE
    s_single_test_drawn = 0u;
#endif
#if APP_CAMERA_PREVIEW_TEST_MODE == APP_CAMERA_PREVIEW_TEST_COLOR
    s_color_test_drawn = 0u;
#endif
    s_frozen = 0;
    if (s_page_active != 0 && service_camera_set_rgb565_mode() == 0) {
        (void)app_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
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
        /*
         * 如果用户点击上传后立即跳到 AI 页面，保留 upload 请求和 JPEG 缓冲，
         * 让后台任务继续完成上传；普通退出则清理暂存图像。
         */
        if (s_upload_req == 0) {
            s_capture_req = 0;
            s_retake_req = 1;
        }
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
