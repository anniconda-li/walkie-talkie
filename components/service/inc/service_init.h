/**
 * @file service_init.h
 * @brief Driver 与 Service 初始化装配入口。
 */
#ifndef SERVICE_INIT_H
#define SERVICE_INIT_H

#include "driver_init.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 当前 service 装配使用 ML307C 网络。 */
#define SERVICE_INIT_NETWORK_BACKEND_ML307C DRIVER_INIT_NETWORK_BACKEND_ML307C
/** @brief 当前 service 装配使用 WiFi STA 网络。 */
#define SERVICE_INIT_NETWORK_BACKEND_WIFI DRIVER_INIT_NETWORK_BACKEND_WIFI
/** @brief 当前 service 装配使用 ES7210 采集。 */
#define SERVICE_INIT_AUDIO_INPUT_ES7210 DRIVER_INIT_AUDIO_INPUT_ES7210
/** @brief 当前 service 装配使用 INMP441 采集。 */
#define SERVICE_INIT_AUDIO_INPUT_INMP441 DRIVER_INIT_AUDIO_INPUT_INMP441
/** @brief 当前 service 装配使用 ES8311 播放。 */
#define SERVICE_INIT_AUDIO_OUTPUT_ES8311 DRIVER_INIT_AUDIO_OUTPUT_ES8311
/** @brief 当前 service 装配使用 MAX98357A 播放。 */
#define SERVICE_INIT_AUDIO_OUTPUT_MAX98357A DRIVER_INIT_AUDIO_OUTPUT_MAX98357A

/** @brief 当前网络后端，默认 ML307C；测试切换时直接改这里的宏值。 */
#ifndef SERVICE_INIT_NETWORK_BACKEND
#define SERVICE_INIT_NETWORK_BACKEND SERVICE_INIT_NETWORK_BACKEND_ML307C
#endif
/** @brief 当前音频输入后端，默认 ES7210；可切换为 SERVICE_INIT_AUDIO_INPUT_INMP441。 */
#ifndef SERVICE_INIT_AUDIO_INPUT_BACKEND
#define SERVICE_INIT_AUDIO_INPUT_BACKEND SERVICE_INIT_AUDIO_INPUT_ES7210
#endif
/** @brief 当前音频输出后端，默认 ES8311；可切换为 SERVICE_INIT_AUDIO_OUTPUT_MAX98357A。 */
#ifndef SERVICE_INIT_AUDIO_OUTPUT_BACKEND
#define SERVICE_INIT_AUDIO_OUTPUT_BACKEND SERVICE_INIT_AUDIO_OUTPUT_ES8311
#endif
/** @brief WiFi STA SSID。 */
#ifndef SERVICE_INIT_WIFI_SSID
#define SERVICE_INIT_WIFI_SSID "WIFI_SSID"
#endif
/** @brief WiFi STA 密码。 */
#ifndef SERVICE_INIT_WIFI_PASSWORD
#define SERVICE_INIT_WIFI_PASSWORD "WIFI_PASSWORD"
#endif

/**
 * @brief 获取 driver 初始化配置。
 *
 * @return driver 初始化配置副本。
 */
driver_init_config_t service_init_get_driver_config(void);

/**
 * @brief 完成 service 能力绑定并初始化所有 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_INIT_H */
