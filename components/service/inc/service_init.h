/**
 * @file service_init.h
 * @brief Driver 与 Service 初始化装配入口。
 */
#ifndef SERVICE_INIT_H
#define SERVICE_INIT_H

#include "service_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 网络 service 后端选择。
 */
typedef enum {
    SERVICE_NETWORK_BACKEND_WIFI = 0, /**< WiFi STA 后端。 */
    SERVICE_NETWORK_BACKEND_4G = 1,   /**< ML307C 4G 后端。 */
} service_network_backend_t;

/**
 * @brief 完成 service 能力绑定并初始化所有 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init(void);

/**
 * @brief 仅完成屏幕 service 能力绑定并初始化屏幕 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_screen(void);

/**
 * @brief 仅完成音频 service 能力绑定并初始化音频 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_audio(void);

/**
 * @brief 仅完成电池 service 能力绑定并初始化电池 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_battery(void);

/**
 * @brief 仅完成网络 service 能力绑定并初始化网络 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_network(void);

/**
 * @brief 按指定后端重新装配网络 service。
 *
 * @param[in] backend 网络后端。
 * @return 成功返回 0；失败返回负值。
 */
int service_init_network_for(service_network_backend_t backend);

/**
 * @brief 获取当前网络 service 后端。
 *
 * @return 当前后端。
 */
service_network_backend_t service_network_get_backend(void);

/**
 * @brief 尝试恢复网络 driver 并重新装配网络 service。
 *
 * 用于运行期网络状态任务在初始化失败或掉线后后台重试。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_network_recover(void);

/**
 * @brief 仅完成摄像头 service 能力绑定并初始化摄像头 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_camera(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_INIT_H */
