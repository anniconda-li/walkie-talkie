/**
 * @file app_ai_ws.h
 * @brief AI 语音与相机共用的持久 WebSocket 传输层。
 */
#ifndef APP_AI_WS_H
#define APP_AI_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_AI_WS_SESSION_BYTES       64u
#define APP_AI_WS_REQUEST_ID_BYTES    80u
#define APP_AI_WS_SHA256_TEXT_BYTES   65u
#define APP_AI_WS_ANSWER_TEXT_BYTES   1024u
#define APP_AI_WS_STATUS_BYTES        24u
#define APP_AI_WS_ERR_CANCELLED       (-900)

/** @brief AI 文本结果提前就绪时的通知；返回 0 表示已经交付给上层。 */
typedef int (*app_ai_ws_text_ready_cb_t)(const char *answer_text, void *ctx);

typedef struct {
    char session[APP_AI_WS_SESSION_BYTES];
    char status[APP_AI_WS_STATUS_BYTES];
    char answer_text[APP_AI_WS_ANSWER_TEXT_BYTES];
    uint32_t reply_bytes;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t bitrate;
    uint8_t channels;
    uint8_t no_speech;
    uint8_t text_delivered;
} app_ai_ws_voice_result_t;

/** @brief 创建 AI WebSocket 唯一读写任务；网络未连接时只等待，不主动猛连。 */
int app_ai_ws_start(void);

/**
 * @brief 上传一份 AOP1 语音、等待问答结果并把完整 ROP1 回复下载到 PSRAM。
 *
 * 该调用在调用任务中同步等待，但 WebSocket socket 始终只由 biz_ai_ws 任务读写。
 */
int app_ai_ws_voice_request(const char *request_id,
                            const uint8_t *audio,
                            uint32_t audio_len,
                            const char sha256[APP_AI_WS_SHA256_TEXT_BYTES],
                            uint8_t *reply,
                            uint32_t reply_capacity,
                            app_ai_ws_text_ready_cb_t text_ready_cb,
                            void *text_ready_ctx,
                            app_ai_ws_voice_result_t *result);

/** @brief 上传 JPEG 并等待服务器返回最终图像分析 JSON。 */
int app_ai_ws_camera_request(const char *request_id,
                             const uint8_t *jpeg,
                             uint32_t jpeg_len,
                             const char sha256[APP_AI_WS_SHA256_TEXT_BYTES],
                             uint8_t *result_json,
                             uint32_t result_capacity,
                             uint32_t *result_len);

/** @brief 中止当前 AI WebSocket 业务；网络不可用时取消消息会在重连后补发。 */
int app_ai_ws_cancel_active(void);

/** @brief 网络模式或地址变化，关闭旧 socket 并让任务按新网络状态重建。 */
void app_ai_ws_network_changed(void);

/** @brief OTA 维护期间关闭连接并停止重连。 */
void app_ai_ws_suspend(void);

/** @brief OTA 维护结束后恢复按需重连。 */
void app_ai_ws_resume(void);

/** @brief 当前是否已经完成 WebSocket 握手并收到 wai1 hello。 */
int app_ai_ws_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_WS_H */
