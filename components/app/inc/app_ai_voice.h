/**
 * @file app_ai_voice.h
 * @brief AI 语音问答业务。
 *
 * 对外只暴露“启动模块、开始录音、停止录音”三个动作。录音结束后的
 * WAV 打包、HTTP 分片上传、响应 WAV 分片拉取和播放都在模块内部后台任务完成。
 */
#ifndef APP_AI_VOICE_H
#define APP_AI_VOICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 AI 语音问答后台任务。
 *
 * 该接口由 app_business_start() 在开机业务启动阶段调用。启动成功后，
 * 模块会创建 AI 后台任务并准备 WAV 收发缓冲区；后续 UI 长按 AI 按钮
 * 时才能调用 record_start/record_stop。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_ai_voice_start(void);

/**
 * @brief 开始 AI 问答录音。
 *
 * UI 长按 AI 按钮时调用。若当前已有 PTT 或其他音频会话占用，则本次请求会被忽略。
 * 本函数不阻塞等待录音完成，只负责抢占音频会话并唤醒 AI 任务采集 PCM。
 */
void app_ai_voice_record_start(void);

/**
 * @brief 停止 AI 问答录音。
 *
 * UI 松开 AI 按钮时调用；后台任务会把已录 PCM 打包成 WAV 并分片上传。
 * 上传和播放在后台异步完成，调用方不需要等待 HTTP 返回。
 */
void app_ai_voice_record_stop(void);

void app_ai_voice_request_reply_play(void);

void app_ai_voice_request_reply_stop(void);

/**
 * @brief 中止当前 AI 问答任务。
 *
 * 可在录音、上传、结果轮询、回复下载或播放阶段调用。本地会立即设置取消标志，
 * 已创建后端 session 时会尽快发送 /ai/cancel 通知。
 *
 * @return 成功返回 0；模块未启动或无可取消任务返回负值。
 */
esp_err_t app_ai_voice_cancel_current(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_VOICE_H */
