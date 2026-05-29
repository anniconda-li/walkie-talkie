/**
 * @file d_config.h
 * @brief Driver 层公共配置和日志宏。
 */
#ifndef D_CONFIG_H
#define D_CONFIG_H

#include "osal_log.h"

/** @brief Driver 调试模式开关。 */
#define D_DEBUG 1

#if D_DEBUG
/** @brief Driver 信息日志宏。 */
#define D_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)
/** @brief Driver 警告日志宏。 */
#define D_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)
/** @brief Driver 错误日志宏。 */
#define D_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define D_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define D_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define D_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

/** @brief 初始化 ML307C 网络驱动。 */
#define D_INIT_NETWORK_ML307C 1
/** @brief 初始化 WiFi STA 网络驱动。 */
#define D_INIT_NETWORK_WIFI 2
/** @brief 初始化 ES7210 + ES8311 音频驱动。 */
#define D_INIT_AUDIO_ES 1
/** @brief 初始化 INMP441 + MAX98357A 音频驱动。 */
#define D_INIT_AUDIO_I2S 2

/** @brief 是否初始化摄像头驱动，0=关闭，1=开启。 */
#ifndef D_INIT_ENABLE_CAMERA
#define D_INIT_ENABLE_CAMERA 1
#endif

/** @brief 当前 driver 网络初始化方案，临时切到 ML307C 便于 4G 测试。 */
#ifndef D_INIT_NETWORK
#define D_INIT_NETWORK D_INIT_NETWORK_ML307C
#endif
/** @brief 当前 driver 音频初始化方案，默认 ES7210 + ES8311。 */
#ifndef D_INIT_AUDIO
#define D_INIT_AUDIO D_INIT_AUDIO_I2S
#endif

#if D_INIT_NETWORK == D_INIT_NETWORK_WIFI
/** @brief WiFi STA SSID，仅启用 WiFi 驱动时有效。 */
#ifndef D_INIT_WIFI_SSID
#define D_INIT_WIFI_SSID "14"
#endif
/** @brief WiFi STA 密码，仅启用 WiFi 驱动时有效。 */
#ifndef D_INIT_WIFI_PASSWORD
#define D_INIT_WIFI_PASSWORD "12345678"
#endif
#endif

#endif /* D_CONFIG_H */
