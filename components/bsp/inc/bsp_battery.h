/**
 * @file bsp_battery.h
 * @brief 电池电量检测 BSP 接口。
 */
#ifndef BSP_BATTERY_H
#define BSP_BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池检测 GPIO 和 ADC。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_battery_init(void);

/**
 * @brief 释放电池检测 ADC 资源并关闭检测电路。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_battery_deinit(void);

/**
 * @brief 读取 BAT_ADC 引脚上的分压后电压。
 *
 * 读取期间会将 BAT_ADC_EN 拉低，使能硬件分压检测；读取完成后拉高关闭。
 *
 * @param[out] voltage_mv ADC 引脚电压，单位 mV。
 * @return 成功返回 0；失败返回负值。
 */
int bsp_battery_read_voltage_mv(int *voltage_mv);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BATTERY_H */
