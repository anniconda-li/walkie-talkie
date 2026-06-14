/**
 * @file d_power_control.h
 * @brief 整板电源保持与电源键关机控制接口。
 */
#ifndef D_POWER_CONTROL_H
#define D_POWER_CONTROL_H

#include "d_config.h"
#include "wdriver_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define d_power_control_KEY_IO WDRIVER_POWER_KEY_IO
#define d_power_control_HOLD_IO WDRIVER_POWER_HOLD_IO

/**
 * @brief 初始化电源保持输出和电源键输入。
 *
 * 会立即将 HOLD_IO 拉高，接管整板供电保持。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_power_control_init(void);

/**
 * @brief 启动电源键监测任务。
 *
 * 启动时如果电源键仍被按住，会先等待松开，避免开机长按被误判为关机长按。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_power_control_start_monitor(void);

/**
 * @brief 拉低供电保持脚，请求硬件断电。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_power_control_shutdown(void);

/**
 * @brief 读取电源键当前状态。
 *
 * @return 1=按下；0=释放；负值=读取失败。
 */
int d_power_control_is_key_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* D_POWER_CONTROL_H */
