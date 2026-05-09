/**
 * @file app_ai_voice.h
 * @brief AI 语音问答业务。
 */
#ifndef APP_AI_VOICE_H
#define APP_AI_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 AI 语音问答后台任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_ai_voice_start(void);

/**
 * @brief 开始 AI 问答录音。
 *
 * UI 长按 AI 按钮时调用。若当前已有 PTT 或其他音频会话占用，则本次请求会被忽略。
 */
void app_ai_voice_record_start(void);

/**
 * @brief 停止 AI 问答录音。
 *
 * UI 松开 AI 按钮时调用；后台任务会把已录 PCM 打包成 WAV 并上传。
 */
void app_ai_voice_record_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_VOICE_H */
