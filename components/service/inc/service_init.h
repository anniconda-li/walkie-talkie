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
 * @brief 完成 service 能力绑定并初始化所有 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init(void);

/**
 * @brief 仅完成网络 service 能力绑定并初始化网络 service。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_init_network(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_INIT_H */
