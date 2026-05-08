/**
 * @file service_battery.c
 * @brief 电池电量服务实现。
 */
#include "service_battery.h"

#include "bsp_battery.h"
#include "service_common.h"

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "service_battery";

typedef struct {
    int percent;
    int adc_mv;
} service_battery_curve_point_t;

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

static uint8_t s_battery_inited = 0u;
static uint8_t s_filter_valid = 0u;
static int s_filtered_adc_mv = 0;

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

static int service_battery_round_percent(int percent)
{
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

int service_battery_init(void)
{
    int ret = bsp_battery_init();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "电池服务初始化失败, ret=%d", ret);
        s_battery_inited = 0u;
        return ret;
    }

    s_battery_inited = 1u;
    return 0;
}

int service_battery_deinit(void)
{
    int ret = bsp_battery_deinit();
    s_battery_inited = 0u;
    s_filter_valid = 0u;
    s_filtered_adc_mv = 0;
    return ret;
}

int service_battery_get_adc_voltage_mv(int *voltage_mv)
{
    if (voltage_mv == NULL) {
        return -1;
    }

    if (s_battery_inited == 0u) {
        int ret = service_battery_init();
        if (ret != 0) {
            return ret;
        }
    }

    return bsp_battery_read_voltage_mv(voltage_mv);
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

    *voltage_mv = filtered_mv;
    *percent = service_battery_round_percent(raw_percent);
    SERVICE_LOGI(TAG,
                 "电池电量: adc=%dmV, filtered=%dmV, percent=%d%%",
                 adc_mv,
                 filtered_mv,
                 *percent);
    return 0;
}
