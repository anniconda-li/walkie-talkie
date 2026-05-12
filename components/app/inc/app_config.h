/**
 * @file app_config.h
 * @brief App 层公共配置、业务常量和日志宏。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "osal_log.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief App 调试模式开关。
 *
 * 定义为 1 时开启调试日志，定义为 0 时关闭调试日志。
 */
#define APP_DEBUG 1

#if APP_DEBUG
/** @brief App 信息日志宏。 */
#define APP_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)
/** @brief App 警告日志宏。 */
#define APP_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)
/** @brief App 错误日志宏。 */
#define APP_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
/** @brief App 信息日志空实现。 */
#define APP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
/** @brief App 警告日志空实现。 */
#define APP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
/** @brief App 错误日志空实现。 */
#define APP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

/** @brief 本机注册到服务器的设备名。 */
#define APP_BUSINESS_DEVICE_NAME        "walkie-01"
/** @brief UDP 对讲服务器地址。 */
#define APP_BUSINESS_SERVER_HOST        "10.212.141.251"
/** @brief UDP 对讲服务器端口。 */
#define APP_BUSINESS_UDP_PORT           9000
/** @brief AI WAV 问答 HTTP 上传地址。 */
#define APP_BUSINESS_AI_HTTP_URL        "http://10.212.141.28:8000/voice_chat_binary_with_audio?language=zh"
/** @brief AI HTTP 请求等待响应的超时时间，单位 ms。 */
#define APP_AI_HTTP_RESPONSE_TIMEOUT_MS 50000u
/** @brief 开机默认频道号。 */
#define APP_BUSINESS_DEFAULT_CHANNEL    1
/** @brief 业务统一 PCM 采样率，单位 Hz。 */
#define APP_BUSINESS_AUDIO_SAMPLE_RATE  16000u
/** @brief 业务统一 PCM 位宽。 */
#define APP_BUSINESS_AUDIO_BITS         16u
/** @brief 业务统一 PCM 声道数，固定单声道。 */
#define APP_BUSINESS_AUDIO_CHANNELS     1u
/** @brief UDP 对讲单包 20ms PCM 样本数。 */
#define APP_BUSINESS_FRAME_SAMPLES      320u
/** @brief UDP 对讲单包 PCM 字节数。 */
#define APP_BUSINESS_FRAME_BYTES        (APP_BUSINESS_FRAME_SAMPLES * sizeof(int16_t))
/** @brief AI 单次录音最长时长，单位 ms。 */
#define APP_BUSINESS_AI_MAX_MS          2000u
/** @brief AI 单次录音最大样本数。 */
#define APP_BUSINESS_AI_MAX_SAMPLES     ((APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AI_MAX_MS) / 1000u)
/** @brief 标准 PCM WAV 文件头长度。 */
#define APP_BUSINESS_WAV_HEADER_LEN     44u
/** @brief AI 录音 WAV 请求和响应复用缓冲区最大字节数。 */
#define APP_BUSINESS_AI_WAV_MAX_BYTES   (APP_BUSINESS_WAV_HEADER_LEN + (APP_BUSINESS_AI_MAX_SAMPLES * sizeof(int16_t)))

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
