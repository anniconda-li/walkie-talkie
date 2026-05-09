/**
 * @file app_business.h
 * @brief 第一版业务控制器入口。
 */
#ifndef APP_BUSINESS_H
#define APP_BUSINESS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 service 并启动业务后台任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_business_start(void);

/**
 * @brief 尝试占用业务音频会话。
 *
 * PTT 和 AI 问答通过该接口互斥，避免同时读写音频设备。
 *
 * @return 成功占用返回 0；已占用或异常返回负值。
 */
int app_business_audio_session_try_begin(void);

/**
 * @brief 释放业务音频会话占用。
 */
void app_business_audio_session_end(void);

/**
 * @brief 查询业务音频会话是否正被占用。
 *
 * @return 被占用返回 1；空闲返回 0。
 */
int app_business_audio_session_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUSINESS_H */
