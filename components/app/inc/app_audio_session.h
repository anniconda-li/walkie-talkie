/**
 * @file app_audio_session.h
 * @brief App 内部音频业务互斥状态。
 */
#ifndef APP_AUDIO_SESSION_H
#define APP_AUDIO_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

int app_audio_session_init(void);
int app_audio_session_try_begin(void);
void app_audio_session_end(void);
int app_audio_session_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_AUDIO_SESSION_H */
