/**
 * @file app_business_config.h
 * @brief 第一版业务配置常量。
 */
#ifndef APP_BUSINESS_CONFIG_H
#define APP_BUSINESS_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 本机注册到服务器的设备名。 */
#define APP_BUSINESS_DEVICE_NAME        "walkie-01"
/** @brief UDP 对讲服务器地址。 */
#define APP_BUSINESS_SERVER_HOST        "192.168.1.100"
/** @brief UDP 对讲服务器端口。 */
#define APP_BUSINESS_UDP_PORT           9000
/** @brief AI WAV 问答 HTTP 上传地址。 */
#define APP_BUSINESS_AI_HTTP_URL        "http://192.168.1.100:8080/ai/wav"
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

#endif /* APP_BUSINESS_CONFIG_H */
