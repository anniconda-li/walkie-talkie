/**
 * @file driver_init.h
 * @brief Driver 层统一初始化入口。
 */
#ifndef DRIVER_INIT_H
#define DRIVER_INIT_H

#include "driver_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化当前项目使用的所有 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_init(void);

/**
 * @brief 仅初始化当前配置选择的网络 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_network_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_INIT_H */
