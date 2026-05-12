/**
 * @file service_battery.c
 * @brief 电池电量服务实现。
 *
 * 下层 driver 只提供 ADC 分压后的电压 mV；service 负责一阶低通滤波、
 * 按锂电池放电曲线映射百分比，并做 5% 步进取整，降低 UI 抖动。
 */
#include "service_battery.h"

#include "service_config.h"

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "service_battery";

typedef struct {
    int percent; /**< 电量百分比。 */
    int adc_mv;  /**< 分压后 ADC 输入电压，单位 mV。 */
} service_battery_curve_point_t;

/**
 * @brief 电池电量曲线表。
 *
 * 硬件已经做 1/2 分压，因此这里使用 ADC 输入电压范围 1500mV-2100mV，
 * 对应电池真实电压 3.0V-4.2V。表项按电压从高到低排列，便于插值。
 */
static const service_battery_curve_point_t s_battery_curve[] = {
    {100, 2100},
    {90, 2030},
    {80, 1990},
    {70, 1960},
    {60, 1935},
    {50, 1910},
    {40, 1895},
    {30, 1885},
    {20, 1870},
    {10, 1840},
    {5, 1725},
    {0, 1500},
};

#define SERVICE_BATTERY_FILTER_ALPHA_NUM    1
#define SERVICE_BATTERY_FILTER_ALPHA_DEN    4
#define SERVICE_BATTERY_PERCENT_STEP        5

/** @brief 电池服务是否已经绑定采样 ops。 */
static uint8_t s_battery_inited = 0u;

/** @brief 滤波器是否已有历史值。首次采样直接采用原始电压。 */
static uint8_t s_filter_valid = 0u;

/** @brief 一阶低通滤波后的 ADC 电压，单位 mV。 */
static int s_filtered_adc_mv = 0;

/** @brief 当前绑定的电池采样 driver 能力函数表。 */
static service_battery_sample_ops_t s_battery_ops;

/**
 * @brief 对 ADC 电压做一阶低通滤波。
 *
 * 滤波公式：filtered = old * 3/4 + new * 1/4。
 * 这样可以压住 1% 精度显示时的跳动，同时保留较快的电量变化响应。
 *
 * @param[in] adc_mv 当前采样到的 ADC 电压，单位 mV。
 * @return 滤波后的 ADC 电压，单位 mV。
 */
static int service_battery_filter_voltage(int adc_mv)
{
    if (s_filter_valid == 0u) {
        s_filtered_adc_mv = adc_mv;
        s_filter_valid = 1u;
        return s_filtered_adc_mv;
    }

    s_filtered_adc_mv =
        ((s_filtered_adc_mv * (SERVICE_BATTERY_FILTER_ALPHA_DEN - SERVICE_BATTERY_FILTER_ALPHA_NUM)) +
         (adc_mv * SERVICE_BATTERY_FILTER_ALPHA_NUM) +
         (SERVICE_BATTERY_FILTER_ALPHA_DEN / 2)) /
        SERVICE_BATTERY_FILTER_ALPHA_DEN;

    return s_filtered_adc_mv;
}

/**
 * @brief 将百分比按固定步进取整并限制到 0-100。
 *
 * @param[in] percent 原始百分比。
 * @return 取整后的百分比。
 */
static int service_battery_round_percent(int percent)
{
    /* UI 只显示 5% 步进，避免电压轻微波动导致百分比频繁变化。 */
    int rounded = ((percent + (SERVICE_BATTERY_PERCENT_STEP / 2)) /
                   SERVICE_BATTERY_PERCENT_STEP) *
                  SERVICE_BATTERY_PERCENT_STEP;

    if (rounded < 0) {
        return 0;
    }
    if (rounded > 100) {
        return 100;
    }

    return rounded;
}

/**
 * @brief 将分压后的 ADC 电压映射为电量百分比。
 *
 * 按 s_battery_curve 查找相邻区间，并在区间内线性插值。
 *
 * @param[in] adc_mv 分压后 ADC 电压，单位 mV。
 * @return 原始电量百分比，范围 0-100。
 */
static int service_battery_voltage_to_percent(int adc_mv)
{
    const uint32_t point_count = sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);

    if (adc_mv >= s_battery_curve[0].adc_mv) {
        return 100;
    }
    if (adc_mv <= s_battery_curve[point_count - 1u].adc_mv) {
        return 0;
    }

    for (uint32_t i = 0; i < point_count - 1u; i++) {
        const service_battery_curve_point_t *high = &s_battery_curve[i];
        const service_battery_curve_point_t *low = &s_battery_curve[i + 1u];

        if (adc_mv <= high->adc_mv && adc_mv >= low->adc_mv) {
            /* 在相邻曲线点之间做线性插值，避免百分比只按表项跳变。 */
            int mv_span = high->adc_mv - low->adc_mv;
            int pct_span = high->percent - low->percent;
            int pct = low->percent;
            if (mv_span > 0) {
                pct += ((adc_mv - low->adc_mv) * pct_span + (mv_span / 2)) / mv_span;
            }
            if (pct < 0) {
                return 0;
            }
            if (pct > 100) {
                return 100;
            }
            return pct;
        }
    }

    return 0;
}

/**
 * @brief 检查电池 service 所需的采样能力是否完整。
 *
 * @param[in] ops 待检查的采样能力函数表。
 * @return 有效返回 0；无效返回 -1。
 */
static int service_battery_ops_is_valid(const service_battery_sample_ops_t *ops)
{
    /* service 只要求 driver 能判断是否初始化，并读取一次 ADC 输入电压。 */
    if (ops == NULL || ops->is_initialized == NULL || ops->read_voltage_mv == NULL) {
        return -1;
    }

    return 0;
}

int service_battery_init(const service_battery_config_t *cfg)
{
    /*
     * cfg == NULL 表示只检查当前 service 是否已初始化。
     * 便于上层在不重新装配 ops 的情况下做容错检查。
     */
    if (cfg == NULL) {
        return s_battery_inited != 0u ? 0 : -2;
    }

    if (service_battery_ops_is_valid(&cfg->sample_ops) != 0) {
        SERVICE_LOGE(TAG, "电池服务初始化失败: ops 无效");
        s_battery_inited = 0u;
        return -3;
    }

    if (cfg->sample_ops.is_initialized() != 1) {
        SERVICE_LOGE(TAG, "电池服务初始化失败: 下层电池 driver 未初始化");
        s_battery_inited = 0u;
        return -4;
    }

    s_battery_ops = cfg->sample_ops;
    s_battery_inited = 1u;
    return 0;
}

int service_battery_deinit(void)
{
    s_battery_inited = 0u;
    s_filter_valid = 0u;
    s_filtered_adc_mv = 0;
    s_battery_ops = (service_battery_sample_ops_t){0};
    return 0;
}

int service_battery_get_adc_voltage_mv(int *voltage_mv)
{
    if (voltage_mv == NULL) {
        return -1;
    }

    if (s_battery_inited == 0u) {
        int ret = service_battery_init(NULL);
        if (ret != 0) {
            return ret;
        }
    }

    return s_battery_ops.read_voltage_mv(voltage_mv);
}

int service_battery_get_percent(int *percent)
{
    if (percent == NULL) {
        return -1;
    }

    int adc_mv = 0;
    return service_battery_get_status(&adc_mv, percent);
}

int service_battery_get_status(int *voltage_mv, int *percent)
{
    if (voltage_mv == NULL || percent == NULL) {
        return -1;
    }

    int adc_mv = 0;
    int ret = service_battery_get_adc_voltage_mv(&adc_mv);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "电池电压读取失败, ret=%d", ret);
        return ret;
    }

    int filtered_mv = service_battery_filter_voltage(adc_mv);
    int raw_percent = service_battery_voltage_to_percent(filtered_mv);

    /* 对外返回滤波后的电压和已取整的百分比，保证同一次采样数据一致。 */
    *voltage_mv = filtered_mv;
    *percent = service_battery_round_percent(raw_percent);
    return 0;
}
