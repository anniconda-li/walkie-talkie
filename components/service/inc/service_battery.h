/**
 * @file service_battery.h
 * @brief 电池电量服务接口。
 *
 * 下层 driver 只提供分压后的 ADC 电压采样；service 负责滤波、
 * 电压到百分比曲线映射、UI 友好的百分比取整和显示滞回。
 * 上层只消费最终电量百分比，不感知 ADC 电压。
 */
#ifndef SERVICE_BATTERY_H
#define SERVICE_BATTERY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电池服务依赖的下层采样能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层电池采样 driver 是否已初始化。 */
    int (*read_voltage_mv)(int *voltage_mv); /**< 读取 ADC 输入电压，单位 mV。 */
} service_battery_sample_ops_t;

/**
 * @brief 初始化电池电量服务。
 *
 * 初始化时会复制 ops 中的采样能力函数表，并检查下层 driver 是否已初始化。
 *
 * @param[in] ops 电池采样能力函数表；为 NULL 时仅检查 service 是否已初始化。
 * @return 成功返回 0；失败返回负值。
 */
int service_battery_init(const service_battery_sample_ops_t *ops);

/**
 * @brief 释放电池电量服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_battery_deinit(void);

/**
 * @brief 获取电池电量。
 *
 * 内部会读取一次 ADC 电压并经过滤波/曲线映射，返回稳定后的百分比。
 *
 * @param[out] level 电量百分比，范围 0 到 100。
 * @return 成功返回 0；失败返回负值。
 */
int service_battery_get(int *level);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_BATTERY_H */
