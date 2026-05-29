/**
 * @file d_battery.h
 * @brief 电池电量检测 WDRIVER 接口。
 */
#ifndef D_BATTERY_H
#define D_BATTERY_H

#include <stdint.h>

#include "d_config.h"
#include "wdriver_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define d_battery_ADC_IO WDRIVER_BATTERY_ADC_IO
#define d_battery_ADC_EN_IO WDRIVER_BATTERY_ADC_EN_IO

/**
 * @brief 初始化电池检测 GPIO 和 ADC。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_battery_init(void);

/**
 * @brief 释放电池检测 ADC 资源并关闭检测电路。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_battery_deinit(void);

/**
 * @brief 判断电池采样驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int d_battery_is_initialized(void);

/**
 * @brief 读取 BAT_ADC 引脚上的分压后电压。
 *
 * 读取期间会将 BAT_ADC_EN 拉低，使能硬件分压检测；读取完成后拉高关闭。
 *
 * @param[out] voltage_mv ADC 引脚电压，单位 mV。
 * @return 成功返回 0；失败返回负值。
 */
int d_battery_read_voltage_mv(int *voltage_mv);

#ifdef __cplusplus
}
#endif

#endif /* D_BATTERY_H */
