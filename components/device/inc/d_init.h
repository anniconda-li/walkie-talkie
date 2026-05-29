/**
 * @file d_init.h
 * @brief Driver 层统一初始化入口。
 */
#ifndef D_INIT_H
#define D_INIT_H

#include "d_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化当前项目使用的所有 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_init(void);

/**
 * @brief 仅初始化屏幕链路依赖的 IO 扩展器、LCD 和触摸 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_screen_init(void);

/**
 * @brief 仅初始化当前配置选择的音频 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_audio_init(void);

/**
 * @brief 仅初始化电池采样 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_power_init(void);

/**
 * @brief 仅初始化当前配置选择的网络 driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_network_init(void);

/**
 * @brief 仅初始化摄像头 driver。
 *
 * @return 成功返回 0；失败返回负值；未启用摄像头时返回 -1。
 */
int d_optional_camera_init(void);

#ifdef __cplusplus
}
#endif

#endif /* D_INIT_H */
