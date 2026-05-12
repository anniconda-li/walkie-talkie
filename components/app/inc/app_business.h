/**
 * @file app_business.h
 * @brief 第一版业务控制器入口。
 *
 * app_business 负责业务启动顺序、UI 事件转发和音频会话互斥。
 * 其他 app 子模块通过这里的音频会话接口避免 PTT 与 AI 同时占用音频链路。
 */
#ifndef APP_BUSINESS_H
#define APP_BUSINESS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 app 层业务控制器。
 *
 * service 初始化已经在 main/service_init 阶段完成，本接口只创建 UI、
 * 注册 UI 回调，并启动状态监听、UDP 对讲和 AI 语音问答任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_business_start(void);

/**
 * @brief 尝试占用业务音频会话。
 *
 * PTT 和 AI 问答通过该接口互斥，避免同时读写音频设备。
 * 该接口为非阻塞语义，当前已有业务占用时直接返回失败。
 *
 * @return 成功占用返回 0；已占用或异常返回负值。
 */
int app_business_audio_session_try_begin(void);

/**
 * @brief 释放业务音频会话占用。
 *
 * 只有成功调用 app_business_audio_session_try_begin() 的业务才应调用本接口。
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
