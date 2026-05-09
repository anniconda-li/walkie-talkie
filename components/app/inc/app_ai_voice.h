/**
 * @file app_ai_voice.h
 * @brief AI 语音问答业务。
 */
#ifndef APP_AI_VOICE_H
#define APP_AI_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

int app_ai_voice_start(void);
void app_ai_voice_record_start(void);
void app_ai_voice_record_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_VOICE_H */
