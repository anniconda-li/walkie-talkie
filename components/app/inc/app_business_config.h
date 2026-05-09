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

#define APP_BUSINESS_DEVICE_NAME        "walkie-01"
#define APP_BUSINESS_SERVER_HOST        "192.168.1.100"
#define APP_BUSINESS_UDP_PORT           9000
#define APP_BUSINESS_AI_HTTP_URL        "http://192.168.1.100:8080/ai/wav"
#define APP_BUSINESS_DEFAULT_CHANNEL    1

#define APP_BUSINESS_AUDIO_SAMPLE_RATE  16000u
#define APP_BUSINESS_AUDIO_BITS         16u
#define APP_BUSINESS_AUDIO_CHANNELS     1u
#define APP_BUSINESS_FRAME_SAMPLES      320u
#define APP_BUSINESS_FRAME_BYTES        (APP_BUSINESS_FRAME_SAMPLES * sizeof(int16_t))
#define APP_BUSINESS_AI_MAX_MS          2000u
#define APP_BUSINESS_AI_MAX_SAMPLES     ((APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AI_MAX_MS) / 1000u)
#define APP_BUSINESS_WAV_HEADER_LEN     44u
#define APP_BUSINESS_AI_WAV_MAX_BYTES   (APP_BUSINESS_WAV_HEADER_LEN + (APP_BUSINESS_AI_MAX_SAMPLES * sizeof(int16_t)))

#ifdef __cplusplus
}
#endif

#endif /* APP_BUSINESS_CONFIG_H */
