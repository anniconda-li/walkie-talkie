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
/** @brief JPEG 上传任务栈大小。 */
#define APP_CAMERA_UPLOAD_TASK_STACK   4096u
/** @brief JPEG 上传任务优先级，低于预览任务。 */
#define APP_CAMERA_UPLOAD_TASK_PRIORITY 3u
/** @brief 模式切换后丢弃帧数，用于等待 sensor 输出稳定。 */
#define APP_CAMERA_MODE_DISCARD_FRAMES 5u
/** @brief RGB565 预览恢复后额外暖机帧数，避免首批异常帧直刷到 LCD。 */
#define APP_CAMERA_RGB565_WARMUP_FRAMES 20u
/** @brief JPEG 模式切换后最多尝试取帧次数。 */
#define APP_CAMERA_JPEG_CAPTURE_TRIES 10u
/** @brief 连续检测到异常预览帧后的相机模式重建阈值。 */
#define APP_CAMERA_BAD_PREVIEW_REJECT_LIMIT 6u
/** @brief 绿屏检测采样步长，使用质数减少固定纹理误判。 */
#define APP_CAMERA_GREEN_SAMPLE_STEP 97u
/** @brief 横纹检测列采样步长。 */
#define APP_CAMERA_STRIPE_SAMPLE_X_STEP 16u
/** @brief 横纹检测相邻行亮度差阈值。 */
#define APP_CAMERA_STRIPE_ROW_DELTA 42
/** @brief 预览黑屏填充一次绘制的行数，避免相机任务栈上出现大缓冲。 */
#define APP_CAMERA_BLACK_CHUNK_LINES 8u

typedef struct {
    uint8_t *jpeg_buf;
    uint32_t jpeg_len;
    uint32_t queued_at_ms;
} app_camera_upload_job_t;

/** @brief 相机后台任务句柄。 */
static osal_task_t s_camera_task = NULL;
/** @brief JPEG 上传任务句柄。 */
static osal_task_t s_upload_task = NULL;
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
/** @brief 当前是否正在处理一次 JPEG 上传，用于切页时保留暂存图像。 */
static volatile int s_upload_in_progress = 0;
/** @brief 当前上传是否已被用户中止；HTTP 返回后据此忽略结果。 */
static volatile int s_upload_cancel_requested = 0;
/** @brief 当前是否有独立 JPEG 上传任务在运行。 */
static volatile int s_upload_task_running = 0;
/** @brief 重拍请求标志，由 UI 回调置位，后台任务消费。 */
static volatile int s_retake_req = 0;
/** @brief 下电请求标志，由页面退出或拍照完成后置位，后台任务消费。 */
static volatile int s_poweroff_req = 0;
/** @brief JPEG 暂存缓冲，分配在外部大容量内存。 */
static uint8_t *s_jpeg_buf = NULL;
/** @brief JPEG 暂存缓冲容量。 */
static uint32_t s_jpeg_buf_size = 0u;
/** @brief 当前暂存 JPEG 有效长度。 */
static uint32_t s_jpeg_len = 0u;
/** @brief HTTP 上传响应临时缓冲。 */
static uint8_t s_upload_resp[APP_CAMERA_UPLOAD_RESP_BYTES];
/** @brief 单次 JPEG 上传任务参数；同一时间只允许一个上传任务。 */
static app_camera_upload_job_t s_upload_job;
/** @brief 上传按钮点击后入队时间。 */
static uint32_t s_upload_queued_at_ms = 0u;
/** @brief RGB565 预览恢复后还需跳过的暖机帧数。 */
static uint8_t s_preview_warmup_frames = 0u;
/** @brief 连续拒绝的疑似异常预览帧数量。 */
static uint8_t s_bad_preview_reject_count = 0u;
/** @brief 预览暖机黑屏分块缓冲，保存在 PSRAM。 */
static uint16_t *s_preview_black_buf = NULL;
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
static int app_camera_build_url(char *out, size_t out_size, const char *path, const char *query)
{
    if (out == NULL || out_size == 0u || path == NULL || query == NULL) {
        return -1;
    }

    const char *base = APP_BUSINESS_HTTP_BASE_URL;
    size_t base_len = strlen(base);
    const char *path_start = path;
    while (*path_start == '/' && base_len > 0u && base[base_len - 1u] == '/') {
        path_start++;
    }

    const char *sep = query[0] != '\0' ? "?" : "";
    int written = snprintf(out, out_size, "%s%s%s%s", base, path_start, sep, query);
    return (written > 0 && (size_t)written < out_size) ? 0 : -2;
}

static int app_camera_json_get_string(const uint8_t *json,
                                      uint32_t len,
                                      const char *key,
                                      char *out,
                                      size_t out_size)
{
    if (json == NULL || key == NULL || out == NULL || out_size == 0u) {
        return -1;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *start = strstr((const char *)json, pattern);
    if (start == NULL || start >= (const char *)json + len) {
        return -2;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        return -3;
    }
    start++;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start != '"') {
        return -4;
    }
    start++;
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return -5;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len >= out_size) {
        copy_len = out_size - 1u;
    }
    memcpy(out, start, copy_len);
    out[copy_len] = '\0';
    return copy_len > 0u ? 0 : -6;
}

static int app_camera_json_is_true(const uint8_t *json, uint32_t len, const char *key)
{
    if (json == NULL || key == NULL) {
        return 0;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *start = strstr((const char *)json, pattern);
    if (start == NULL || start >= (const char *)json + len) {
        return 0;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        return 0;
    }
    start++;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    return strncmp(start, "true", 4u) == 0 ? 1 : 0;
}

static void app_camera_handle_upload_response(const uint8_t *json, uint32_t len)
{
    char answer_text[512];
    answer_text[0] = '\0';

    int ok = app_camera_json_is_true(json, len, "ok");
    int analysis_ok = app_camera_json_is_true(json, len, "analysis_ok");
    (void)app_camera_json_get_string(json, len, "answer_text", answer_text, sizeof(answer_text));

    (void)app_ui_set_ai_waiting(0);
    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);

    if (!ok) {
        APP_LOGW(TAG, "相机图像分析失败: ok=false");
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        return;
    }

    if (!analysis_ok) {
        APP_LOGW(TAG, "相机图像分析要求重拍");
        if (answer_text[0] != '\0') {
            (void)app_ui_set_ai_answer_text(answer_text);
        } else {
            (void)app_ui_set_ai_answer_text("这张照片信息不太够，请重拍。");
        }
        return;
    }

    APP_LOGI(TAG, "相机图像分析完成，可以提问");
    (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_READY);
}

static void app_camera_finish_upload_request(void)
{
    if (app_camera_lock() == 0) {
        s_upload_in_progress = 0;
        s_upload_cancel_requested = 0;
        s_upload_task_running = 0;
        s_upload_task = NULL;
        s_upload_queued_at_ms = 0u;
        app_camera_unlock();
    } else {
        s_upload_in_progress = 0;
        s_upload_cancel_requested = 0;
        s_upload_task_running = 0;
        s_upload_task = NULL;
        s_upload_queued_at_ms = 0u;
    }
}

static int app_camera_is_upload_cancel_requested(void)
{
    return s_upload_cancel_requested != 0;
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

static void app_camera_free_upload_job(app_camera_upload_job_t *job)
{
    if (job == NULL) {
        return;
    }
    if (job->jpeg_buf != NULL) {
        osal_heap_free(job->jpeg_buf);
        job->jpeg_buf = NULL;
    }
    job->jpeg_len = 0u;
    job->queued_at_ms = 0u;
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

static void app_camera_draw_black_preview(void);

static void app_camera_mark_rgb565_warmup(void)
{
    s_preview_warmup_frames = APP_CAMERA_RGB565_WARMUP_FRAMES;
    s_bad_preview_reject_count = 0u;
    app_camera_draw_black_preview();
}

static int app_camera_ensure_black_buffer(void)
{
    const size_t bytes = (size_t)APP_CAMERA_PREVIEW_W *
                         (size_t)APP_CAMERA_BLACK_CHUNK_LINES *
                         sizeof(uint16_t);

    if (s_preview_black_buf != NULL) {
        return 0;
    }

    s_preview_black_buf = (uint16_t *)osal_heap_alloc_external(bytes);
    if (s_preview_black_buf == NULL) {
        APP_LOGW(TAG,
                 "相机黑屏缓冲分配失败, bytes=%u, external_free=%u",
                 (unsigned int)bytes,
                 (unsigned int)osal_heap_get_external_free_size());
        return -1;
    }

    memset(s_preview_black_buf, 0, bytes);
    return 0;
}

static void app_camera_draw_black_preview(void)
{
    if (app_camera_ensure_black_buffer() != 0) {
        return;
    }

    for (uint32_t y = 0u; y < APP_CAMERA_PREVIEW_H; y += APP_CAMERA_BLACK_CHUNK_LINES) {
        uint32_t lines = APP_CAMERA_PREVIEW_H - y;
        if (lines > APP_CAMERA_BLACK_CHUNK_LINES) {
            lines = APP_CAMERA_BLACK_CHUNK_LINES;
        }
        (void)service_screen_draw_rgb565(APP_CAMERA_PREVIEW_X,
                                         APP_CAMERA_PREVIEW_Y + (int)y,
                                         APP_CAMERA_PREVIEW_W,
                                         (int)lines,
                                         s_preview_black_buf);
    }
}

static int app_camera_power_on_for_preview(void)
{
    int ret = service_camera_power_on();
    if (ret != 0) {
        APP_LOGW(TAG, "相机上电失败, ret=%d", ret);
        return ret;
    }

    return 0;
}

static void app_camera_power_off_idle(void)
{
    s_preview_active = 0;
    s_preview_warmup_frames = 0u;
    s_bad_preview_reject_count = 0u;

    int ret = service_camera_power_off();
    if (ret == 0) {
        APP_LOGI(TAG, "相机空闲下电完成");
    } else {
        APP_LOGW(TAG, "相机空闲下电失败, ret=%d", ret);
    }
}

static int app_camera_frame_is_valid_rgb565_preview(const service_camera_frame_t *frame)
{
    if (frame == NULL ||
        frame->format != SERVICE_CAMERA_FORMAT_RGB565 ||
        frame->data == NULL ||
        frame->width != APP_CAMERA_PREVIEW_W ||
        frame->height != APP_CAMERA_PREVIEW_H ||
        frame->len < (APP_CAMERA_PREVIEW_W * APP_CAMERA_PREVIEW_H * sizeof(uint16_t))) {
        return 0;
    }

    return 1;
}

static uint32_t app_camera_rgb565_luma_sum(const uint8_t *data, uint32_t pixel_index, uint8_t order)
{
    const uint32_t byte_index = pixel_index * sizeof(uint16_t);
    uint16_t pixel = (uint16_t)data[byte_index] | ((uint16_t)data[byte_index + 1u] << 8);
    if (order != 0u) {
        pixel = ((uint16_t)data[byte_index] << 8) | (uint16_t)data[byte_index + 1u];
    }

    uint8_t r = (uint8_t)((pixel >> 11) & 0x1Fu);
    uint8_t g = (uint8_t)((pixel >> 5) & 0x3Fu);
    uint8_t b = (uint8_t)(pixel & 0x1Fu);
    return (uint32_t)(r << 1) + (uint32_t)g + (uint32_t)(b << 1);
}

static int app_camera_frame_is_green_screen(const service_camera_frame_t *frame)
{
    if (app_camera_frame_is_valid_rgb565_preview(frame) == 0) {
        return 0;
    }

    const uint32_t pixel_count = APP_CAMERA_PREVIEW_W * APP_CAMERA_PREVIEW_H;
    uint32_t samples = 0u;
    uint32_t strong_green[2] = {0u, 0u};
    uint32_t dominant_green[2] = {0u, 0u};
    uint32_t sum_r6[2] = {0u, 0u};
    uint32_t sum_g6[2] = {0u, 0u};
    uint32_t sum_b6[2] = {0u, 0u};

    for (uint32_t i = 0u; i < pixel_count; i += APP_CAMERA_GREEN_SAMPLE_STEP) {
        uint32_t byte_index = i * sizeof(uint16_t);
        uint16_t pixel[2] = {
            (uint16_t)frame->data[byte_index] | ((uint16_t)frame->data[byte_index + 1u] << 8),
            ((uint16_t)frame->data[byte_index] << 8) | (uint16_t)frame->data[byte_index + 1u],
        };

        samples++;
        for (uint8_t order = 0u; order < 2u; order++) {
            uint8_t r = (uint8_t)((pixel[order] >> 11) & 0x1Fu);
            uint8_t g = (uint8_t)((pixel[order] >> 5) & 0x3Fu);
            uint8_t b = (uint8_t)(pixel[order] & 0x1Fu);
            uint8_t r6 = (uint8_t)(r << 1);
            uint8_t b6 = (uint8_t)(b << 1);

            sum_r6[order] += r6;
            sum_g6[order] += g;
            sum_b6[order] += b6;
            if (g >= 44u && r <= 12u && b <= 12u) {
                strong_green[order]++;
            }
            if (g >= 32u && g >= (uint8_t)(r6 + 6u) && g >= (uint8_t)(b6 + 6u)) {
                dominant_green[order]++;
            }
        }
    }

    if (samples == 0u) {
        return 0;
    }

    for (uint8_t order = 0u; order < 2u; order++) {
        if ((strong_green[order] * 100u) >= (samples * 50u)) {
            return 1;
        }
        if ((dominant_green[order] * 100u) >= (samples * 78u) &&
            sum_g6[order] >= (sum_r6[order] + (samples * 8u)) &&
            sum_g6[order] >= (sum_b6[order] + (samples * 8u))) {
            return 1;
        }
    }

    return 0;
}

static int app_camera_frame_has_horizontal_stripes(const service_camera_frame_t *frame)
{
    if (app_camera_frame_is_valid_rgb565_preview(frame) == 0) {
        return 0;
    }

    uint32_t comparisons = 0u;
    uint32_t strong_delta[2] = {0u, 0u};
    uint32_t alternating_delta[2] = {0u, 0u};
    int32_t previous_delta[2] = {0, 0};

    for (uint32_t y = 0u; y + 1u < APP_CAMERA_PREVIEW_H; y++) {
        uint32_t row_sum[2][2] = {{0u, 0u}, {0u, 0u}};
        uint32_t x_samples = 0u;

        for (uint32_t x = 4u; x < APP_CAMERA_PREVIEW_W; x += APP_CAMERA_STRIPE_SAMPLE_X_STEP) {
            uint32_t pixel0 = (y * APP_CAMERA_PREVIEW_W) + x;
            uint32_t pixel1 = ((y + 1u) * APP_CAMERA_PREVIEW_W) + x;
            row_sum[0][0] += app_camera_rgb565_luma_sum(frame->data, pixel0, 0u);
            row_sum[0][1] += app_camera_rgb565_luma_sum(frame->data, pixel1, 0u);
            row_sum[1][0] += app_camera_rgb565_luma_sum(frame->data, pixel0, 1u);
            row_sum[1][1] += app_camera_rgb565_luma_sum(frame->data, pixel1, 1u);
            x_samples++;
        }

        if (x_samples == 0u) {
            continue;
        }

        comparisons++;
        for (uint8_t order = 0u; order < 2u; order++) {
            int32_t avg0 = (int32_t)(row_sum[order][0] / x_samples);
            int32_t avg1 = (int32_t)(row_sum[order][1] / x_samples);
            int32_t delta = avg0 - avg1;
            int32_t abs_delta = delta >= 0 ? delta : -delta;
            if (abs_delta >= APP_CAMERA_STRIPE_ROW_DELTA) {
                strong_delta[order]++;
                if ((previous_delta[order] > 0 && delta < 0) ||
                    (previous_delta[order] < 0 && delta > 0)) {
                    alternating_delta[order]++;
                }
                previous_delta[order] = delta;
            } else {
                previous_delta[order] = 0;
            }
        }
    }

    if (comparisons == 0u) {
        return 0;
    }

    for (uint8_t order = 0u; order < 2u; order++) {
        if ((strong_delta[order] * 100u) >= (comparisons * 72u) &&
            (alternating_delta[order] * 100u) >= (comparisons * 42u)) {
            return 1;
        }
    }

    return 0;
}

static int app_camera_frame_is_bad_preview(const service_camera_frame_t *frame)
{
    return (app_camera_frame_is_green_screen(frame) != 0 ||
            app_camera_frame_has_horizontal_stripes(frame) != 0) ? 1 : 0;
}

static void app_camera_recover_rgb565_preview(void)
{
    APP_LOGW(TAG, "相机预览连续检测到异常帧，重建 RGB565 模式");
    s_preview_active = 0;
    s_frozen = 0;
    if (s_page_active == 0) {
        app_camera_power_off_idle();
        return;
    }
    app_camera_draw_black_preview();

    if (app_camera_power_on_for_preview() != 0) {
        return;
    }

    (void)service_camera_set_jpeg_mode();
    (void)app_camera_discard_frames(2u);

    if (service_camera_set_rgb565_mode() == 0) {
        (void)app_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
        if (s_page_active != 0) {
            app_camera_mark_rgb565_warmup();
            s_preview_active = 1;
        } else {
            app_camera_power_off_idle();
        }
    } else {
        APP_LOGW(TAG, "相机 RGB565 模式重建失败");
        app_camera_power_off_idle();
    }
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
        if (s_preview_warmup_frames > 0u) {
            s_preview_warmup_frames--;
            service_camera_return_frame(&frame);
            osal_delay_ms(APP_CAMERA_PREVIEW_INTERVAL_MS);
            return;
        }
        if (app_camera_frame_is_bad_preview(&frame) != 0) {
            s_bad_preview_reject_count++;
            if (s_bad_preview_reject_count == 1u ||
                s_bad_preview_reject_count >= APP_CAMERA_BAD_PREVIEW_REJECT_LIMIT) {
                APP_LOGW(TAG,
                         "相机预览疑似异常帧，已跳过, count=%u",
                         (unsigned int)s_bad_preview_reject_count);
            }
            service_camera_return_frame(&frame);
            if (s_bad_preview_reject_count >= APP_CAMERA_BAD_PREVIEW_REJECT_LIMIT) {
                app_camera_recover_rgb565_preview();
            }
            osal_delay_ms(APP_CAMERA_PREVIEW_INTERVAL_MS);
            return;
        }
        s_bad_preview_reject_count = 0u;
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
    if (service_camera_is_initialized() != 1 || app_camera_power_on_for_preview() != 0) {
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
        app_camera_power_off_idle();
        return;
    }

    for (uint32_t i = 0u; i < APP_CAMERA_JPEG_CAPTURE_TRIES; i++) {
        service_camera_frame_t frame;
        ret = service_camera_get_frame(&frame);
        if (ret != 0) {
            APP_LOGW(TAG, "相机 JPEG 取帧失败, try=%u, ret=%d", (unsigned int)i, ret);
            app_camera_power_off_idle();
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
        app_camera_power_off_idle();
        return;
    }

    APP_LOGW(TAG, "相机 JPEG 取帧失败: 连续 %u 帧都不是合法 JPEG",
             (unsigned int)APP_CAMERA_JPEG_CAPTURE_TRIES);
    app_camera_power_off_idle();
}

/**
 * @brief 执行 JPEG 上传。
 */
static void app_camera_do_upload(app_camera_upload_job_t *job)
{
    int ret = 0;

    if (job == NULL || job->jpeg_buf == NULL || job->jpeg_len == 0u) {
        APP_LOGW(TAG, "相机上传失败: 没有可上传的 JPEG");
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        app_camera_finish_upload_request();
        return;
    }

    uint32_t task_start_ms = osal_get_tick_ms();
    APP_LOGI(TAG,
             "相机 JPEG 上传任务启动, len=%u, queued_delay=%u",
             (unsigned int)job->jpeg_len,
             (unsigned int)(task_start_ms - job->queued_at_ms));

    if (app_camera_is_upload_cancel_requested()) {
        APP_LOGI(TAG, "相机上传已中止: 请求开始前取消");
        app_camera_free_upload_job(job);
        app_camera_finish_upload_request();
        return;
    }

    if (service_network_is_ready() != 1) {
        APP_LOGW(TAG, "相机上传失败: 网络未就绪");
        app_camera_free_upload_job(job);
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_NO_NETWORK);
        app_camera_finish_upload_request();
        return;
    }

    uint32_t resp_len = 0u;
    char url[256];
    char query[96];
    snprintf(query, sizeof(query), "device=%s", APP_DEVICE_ID);
    ret = app_camera_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_CAMERA_UPLOAD, query);
    if (ret != 0) {
        APP_LOGW(TAG, "相机上传 URL 构造失败, ret=%d", ret);
        app_camera_free_upload_job(job);
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        app_camera_finish_upload_request();
        return;
    }

    for (uint32_t attempt = 0u; attempt <= APP_CAMERA_UPLOAD_RETRY_COUNT; attempt++) {
        if (app_camera_is_upload_cancel_requested()) {
            APP_LOGI(TAG, "相机上传已中止: 跳过剩余重试");
            app_camera_free_upload_job(job);
            app_camera_finish_upload_request();
            return;
        }
        resp_len = 0u;
        uint32_t http_start_ms = osal_get_tick_ms();
        APP_LOGI(TAG,
                 "相机 JPEG HTTP POST 开始, attempt=%u/%u, len=%u, since_click=%u",
                 (unsigned int)(attempt + 1u),
                 (unsigned int)(APP_CAMERA_UPLOAD_RETRY_COUNT + 1u),
                 (unsigned int)job->jpeg_len,
                 (unsigned int)(http_start_ms - job->queued_at_ms));
        ret = service_network_http_post(url,
                                        "image/jpeg",
                                        job->jpeg_buf,
                                        job->jpeg_len,
                                        s_upload_resp,
                                        sizeof(s_upload_resp) - 1u,
                                        &resp_len,
                                        APP_CAMERA_UPLOAD_TIMEOUT_MS);
        uint32_t http_ms = osal_get_tick_ms() - http_start_ms;
        if (ret == 0) {
            APP_LOGI(TAG,
                     "相机 JPEG HTTP POST 返回, attempt=%u/%u, http_ms=%u, resp_len=%u",
                     (unsigned int)(attempt + 1u),
                     (unsigned int)(APP_CAMERA_UPLOAD_RETRY_COUNT + 1u),
                     (unsigned int)http_ms,
                     (unsigned int)resp_len);
            break;
        }
        if (app_camera_is_upload_cancel_requested()) {
            APP_LOGI(TAG, "相机上传已中止: HTTP 返回后忽略失败");
            app_camera_free_upload_job(job);
            app_camera_finish_upload_request();
            return;
        }
        APP_LOGW(TAG,
                 "相机 JPEG 上传失败, attempt=%u/%u, ret=%d, len=%u, http_ms=%u",
                 (unsigned int)(attempt + 1u),
                 (unsigned int)(APP_CAMERA_UPLOAD_RETRY_COUNT + 1u),
                 ret,
                 (unsigned int)job->jpeg_len,
                 (unsigned int)http_ms);
        if (attempt < APP_CAMERA_UPLOAD_RETRY_COUNT) {
            osal_delay_ms(APP_CAMERA_UPLOAD_RETRY_DELAY_MS);
        }
    }

    if (app_camera_is_upload_cancel_requested()) {
        APP_LOGI(TAG, "相机上传已中止: 忽略 HTTP 结果");
        app_camera_free_upload_job(job);
        app_camera_finish_upload_request();
        return;
    }

    if (ret == 0) {
        s_upload_resp[resp_len < sizeof(s_upload_resp) ? resp_len : (sizeof(s_upload_resp) - 1u)] = '\0';
        APP_LOGI(TAG,
                 "相机 JPEG 上传成功, len=%u, resp_len=%u, total_ms=%u",
                 (unsigned int)job->jpeg_len,
                 (unsigned int)resp_len,
                 (unsigned int)(osal_get_tick_ms() - job->queued_at_ms));
        app_camera_handle_upload_response(s_upload_resp, resp_len);
    } else {
        APP_LOGW(TAG,
                 "相机 JPEG 上传失败, ret=%d, len=%u, total_ms=%u",
                 ret,
                 (unsigned int)job->jpeg_len,
                 (unsigned int)(osal_get_tick_ms() - job->queued_at_ms));
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
    }

    app_camera_free_upload_job(job);
    app_camera_finish_upload_request();
}

static void app_camera_upload_task(void *arg)
{
    if (arg == NULL) {
        app_camera_finish_upload_request();
        osal_task_delete_current();
        return;
    }

    app_camera_upload_job_t job = *(app_camera_upload_job_t *)arg;
    app_camera_do_upload(&job);
    osal_task_delete_current();
}

static void app_camera_start_upload_task(void)
{
    if (app_camera_is_upload_cancel_requested()) {
        APP_LOGI(TAG, "相机上传已中止: 任务创建前取消");
        app_camera_clear_jpeg();
        app_camera_finish_upload_request();
        return;
    }

    if (s_upload_task_running != 0) {
        APP_LOGW(TAG, "相机上传请求忽略: 已有上传任务运行");
        return;
    }

    if (s_jpeg_buf == NULL || s_jpeg_len == 0u) {
        APP_LOGW(TAG, "相机上传失败: 没有可上传的 JPEG");
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        app_camera_finish_upload_request();
        return;
    }

    s_upload_job.jpeg_buf = s_jpeg_buf;
    s_upload_job.jpeg_len = s_jpeg_len;
    s_upload_job.queued_at_ms = s_upload_queued_at_ms != 0u ? s_upload_queued_at_ms : osal_get_tick_ms();
    s_jpeg_buf = NULL;
    s_jpeg_buf_size = 0u;
    s_jpeg_len = 0u;
    s_upload_task_running = 1;

    int ret = osal_task_create("cam_upload",
                               app_camera_upload_task,
                               &s_upload_job,
                               APP_CAMERA_UPLOAD_TASK_STACK,
                               APP_CAMERA_UPLOAD_TASK_PRIORITY,
                               &s_upload_task);
    if (ret != 0) {
        APP_LOGE(TAG, "相机上传任务创建失败, ret=%d", ret);
        app_camera_free_upload_job(&s_upload_job);
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IMAGE_UPLOAD_FAILED);
        app_camera_finish_upload_request();
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
    if (s_page_active == 0) {
        return;
    }
    app_camera_draw_black_preview();
    if (app_camera_power_on_for_preview() != 0) {
        return;
    }
    if (service_camera_set_rgb565_mode() != 0) {
        APP_LOGW(TAG, "相机切换 RGB565 预览模式失败");
        app_camera_power_off_idle();
        return;
    }

    (void)app_camera_discard_frames(APP_CAMERA_MODE_DISCARD_FRAMES);
    if (s_page_active != 0) {
        app_camera_mark_rgb565_warmup();
        s_preview_active = 1;
    } else {
        app_camera_power_off_idle();
    }
}

/**
 * @brief 消费 UI 请求标志。
 *
 * 返回前把标志从全局状态中取出并清零，保证一次 UI 操作只处理一次。
 */
static void app_camera_take_requests(int *capture, int *upload, int *retake, int *poweroff)
{
    *capture = 0;
    *upload = 0;
    *retake = 0;
    *poweroff = 0;

    if (app_camera_lock() != 0) {
        return;
    }
    *capture = s_capture_req;
    *upload = s_upload_req;
    *retake = s_retake_req;
    *poweroff = s_poweroff_req;
    s_capture_req = 0;
    s_upload_req = 0;
    s_retake_req = 0;
    s_poweroff_req = 0;
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
        int poweroff = 0;
        app_camera_take_requests(&capture, &upload, &retake, &poweroff);

        if (retake) {
            app_camera_do_retake();
        }
        if (capture) {
            app_camera_do_capture();
        }
        if (upload) {
            app_camera_start_upload_task();
        }
        if (poweroff) {
            app_camera_power_off_idle();
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
        s_poweroff_req = 0;
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
        s_retake_req = 0;
        s_poweroff_req = 1;
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

int app_camera_upload(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return -1;
    }

    if (app_camera_lock() != 0) {
        return -4;
    }

    if (s_upload_in_progress != 0 || s_upload_task_running != 0) {
        APP_LOGW(TAG, "相机上传请求忽略: 当前已有上传进行中");
        app_camera_unlock();
        return -3;
    }
    if (s_jpeg_buf == NULL || s_jpeg_len == 0u) {
        APP_LOGW(TAG, "相机上传请求拒绝: 没有可上传的 JPEG");
        app_camera_unlock();
        return -2;
    }
    s_upload_req = 1;
    s_upload_in_progress = 1;
    s_upload_cancel_requested = 0;
    s_upload_queued_at_ms = osal_get_tick_ms();
    APP_LOGI(TAG,
             "相机上传请求入队, jpeg_len=%u, tick=%u",
             (unsigned int)s_jpeg_len,
             (unsigned int)s_upload_queued_at_ms);
    app_camera_unlock();
    app_camera_notify_task();
    return 0;
}

int app_camera_cancel_current(void)
{
    if (!s_started) {
        return -1;
    }

    int queued = 0;
    int active = 0;
    if (app_camera_lock() == 0) {
        queued = s_upload_req != 0;
        active = (s_upload_req != 0 || s_upload_in_progress != 0);
        if (active) {
            s_upload_cancel_requested = 1;
            if (queued) {
                s_upload_req = 0;
                s_upload_in_progress = 0;
            }
        }
        app_camera_unlock();
    }

    if (!active) {
        return -2;
    }

    if (queued) {
        app_camera_clear_jpeg();
        app_camera_finish_upload_request();
    }
    APP_LOGI(TAG,
             "相机上传取消请求已接收, queued=%d, task_running=%d",
             queued,
             s_upload_task_running);
    app_camera_notify_task();
    return 0;
}

void app_camera_retake(void)
{
    if (!s_started || service_camera_is_initialized() != 1) {
        return;
    }

    if (app_camera_lock() == 0) {
        s_retake_req = 1;
        s_poweroff_req = 0;
        app_camera_unlock();
    }
    app_camera_notify_task();
}
