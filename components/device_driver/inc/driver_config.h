/**
 * @file driver_config.h
 * @brief Driver 层公共配置和日志宏。
 */
#ifndef DRIVER_CONFIG_H
#define DRIVER_CONFIG_H

#include "osal_log.h"

/** @brief Driver 调试模式开关。 */
#define DRIVER_DEBUG 1

#if DRIVER_DEBUG
/** @brief Driver 信息日志宏。 */
#define DRIVER_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)
/** @brief Driver 警告日志宏。 */
#define DRIVER_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)
/** @brief Driver 错误日志宏。 */
#define DRIVER_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define DRIVER_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define DRIVER_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define DRIVER_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

/** @brief 初始化 ML307C 网络驱动。 */
#define DRIVER_INIT_NETWORK_ML307C 1
/** @brief 初始化 WiFi STA 网络驱动。 */
#define DRIVER_INIT_NETWORK_WIFI 2
/** @brief 初始化 ES7210 + ES8311 音频驱动。 */
#define DRIVER_INIT_AUDIO_ES 1
/** @brief 初始化 INMP441 + MAX98357A 音频驱动。 */
#define DRIVER_INIT_AUDIO_I2S 2

/** @brief 当前 driver 网络初始化方案，默认 ML307C。 */
#ifndef DRIVER_INIT_NETWORK
#define DRIVER_INIT_NETWORK DRIVER_INIT_NETWORK_WIFI
#endif
/** @brief 当前 driver 音频初始化方案，默认 ES7210 + ES8311。 */
#ifndef DRIVER_INIT_AUDIO
#define DRIVER_INIT_AUDIO DRIVER_INIT_AUDIO_I2S
#endif

#if DRIVER_INIT_NETWORK == DRIVER_INIT_NETWORK_WIFI
/** @brief WiFi STA SSID，仅启用 WiFi 驱动时有效。 */
#ifndef DRIVER_INIT_WIFI_SSID
#define DRIVER_INIT_WIFI_SSID "14"
#endif
/** @brief WiFi STA 密码，仅启用 WiFi 驱动时有效。 */
#ifndef DRIVER_INIT_WIFI_PASSWORD
#define DRIVER_INIT_WIFI_PASSWORD "12345678"
#endif
#endif

#endif /* DRIVER_CONFIG_H */
