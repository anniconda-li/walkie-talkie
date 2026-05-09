/**
 * @file driver_init.h
 * @brief Driver 层统一初始化入口。
 */
#ifndef DRIVER_INIT_H
#define DRIVER_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 使用 ML307C 网络驱动。 */
#define DRIVER_INIT_NETWORK_BACKEND_ML307C 1
/** @brief 使用 WiFi STA 网络驱动。 */
#define DRIVER_INIT_NETWORK_BACKEND_WIFI   2
/** @brief 使用 ES7210 作为音频输入。 */
#define DRIVER_INIT_AUDIO_INPUT_ES7210     1
/** @brief 使用 INMP441 作为音频输入。 */
#define DRIVER_INIT_AUDIO_INPUT_INMP441    2
/** @brief 使用 ES8311 作为音频输出。 */
#define DRIVER_INIT_AUDIO_OUTPUT_ES8311    1
/** @brief 使用 MAX98357A 作为音频输出。 */
#define DRIVER_INIT_AUDIO_OUTPUT_MAX98357A 2

/**
 * @brief Driver 层初始化选择。
 *
 * 上层初始化装配代码负责决定启用哪一套 driver，driver 层只按传入配置初始化对应设备。
 */
typedef struct {
    int network_backend;      /**< 网络后端选择。 */
    int audio_input_backend;  /**< 音频输入后端选择。 */
    int audio_output_backend; /**< 音频输出后端选择。 */
    const char *wifi_ssid;    /**< WiFi STA SSID，仅 WiFi 后端使用。 */
    const char *wifi_password; /**< WiFi STA 密码，仅 WiFi 后端使用。 */
} driver_init_config_t;

/**
 * @brief 初始化当前项目使用的所有 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_init(const driver_init_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_INIT_H */
