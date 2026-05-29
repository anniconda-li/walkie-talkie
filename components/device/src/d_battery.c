/**
 * @file d_battery.c
 * @brief 电池电量检测 WDRIVER 实现。
 */
#include "d_battery.h"

#include "d_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "osal_task.h"

static const char *TAG = "d_battery";

#define d_battery_ADC_UNIT              ADC_UNIT_2
#define d_battery_ADC_CHANNEL           ADC_CHANNEL_9
#define d_battery_ADC_ATTEN             ADC_ATTEN_DB_12
#define d_battery_ADC_BITWIDTH          ADC_BITWIDTH_DEFAULT
#define d_battery_SAMPLE_COUNT          32u
#define d_battery_DISCARD_COUNT         4u
#define d_battery_ENABLE_SETTLE_MS      20u

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static uint8_t s_adc_cali_enabled = 0u;

/**
 * @brief 将 ESP-IDF 错误码统一转换为本层负值错误码。
 */
static int d_battery_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/**
 * @brief 初始化电池采样使能 GPIO 和 ADC 输入 GPIO。
 */
static int d_battery_gpio_init(void)
{
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << d_battery_ADC_EN_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    int ret = d_battery_err_to_int(gpio_config(&en_cfg));
    if (ret != 0) {
        D_LOGE(TAG, "电池检测使能脚配置失败, io=%d, ret=%d",
                 d_battery_ADC_EN_IO, ret);
        return ret;
    }

    ret = d_battery_err_to_int(gpio_set_level(d_battery_ADC_EN_IO, 1));
    if (ret != 0) {
        D_LOGE(TAG, "电池检测使能脚关闭失败, io=%d, ret=%d",
                 d_battery_ADC_EN_IO, ret);
        return ret;
    }

    gpio_config_t adc_gpio_cfg = {
        .pin_bit_mask = 1ULL << d_battery_ADC_IO,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = d_battery_err_to_int(gpio_config(&adc_gpio_cfg));
    if (ret != 0) {
        D_LOGE(TAG, "电池 ADC 引脚配置失败, io=%d, ret=%d",
                 d_battery_ADC_IO, ret);
        return ret;
    }

    return 0;
}

/**
 * @brief 尝试创建 ADC 校准句柄，失败时退回原始值估算。
 */
static void d_battery_cali_init(void)
{
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = d_battery_ADC_UNIT,
        .atten = d_battery_ADC_ATTEN,
        .bitwidth = d_battery_ADC_BITWIDTH,
    };

    int ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle);
    if (ret == 0) {
        s_adc_cali_enabled = 1u;
        D_LOGI(TAG, "电池 ADC 校准已启用");
    } else {
        s_adc_cali_enabled = 0u;
        s_adc_cali_handle = NULL;
        D_LOGW(TAG, "电池 ADC 校准不可用, 使用原始值估算, ret=%d", ret);
    }
}

int d_battery_init(void)
{
    if (s_adc_handle != NULL) {
        return 0;
    }

    int ret = d_battery_gpio_init();
    if (ret != 0) {
        return ret;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = d_battery_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ret = d_battery_err_to_int(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));
    if (ret != 0) {
        D_LOGE(TAG, "电池 ADC 单元初始化失败, ret=%d", ret);
        s_adc_handle = NULL;
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = d_battery_ADC_ATTEN,
        .bitwidth = d_battery_ADC_BITWIDTH,
    };

    ret = d_battery_err_to_int(adc_oneshot_config_channel(s_adc_handle,
                                                            d_battery_ADC_CHANNEL,
                                                            &chan_cfg));
    if (ret != 0) {
        D_LOGE(TAG, "电池 ADC 通道配置失败, channel=%d, ret=%d",
                 d_battery_ADC_CHANNEL, ret);
        (void)adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    d_battery_cali_init();
    D_LOGI(TAG, "电池检测初始化完成, adc_io=%d, en_io=%d",
             d_battery_ADC_IO, d_battery_ADC_EN_IO);
    return 0;
}

int d_battery_deinit(void)
{
    int ret = 0;

    (void)gpio_set_level(d_battery_ADC_EN_IO, 1);

    if (s_adc_cali_handle != NULL) {
        int cali_ret = d_battery_err_to_int(adc_cali_delete_scheme_curve_fitting(s_adc_cali_handle));
        if (ret == 0 && cali_ret != 0) {
            ret = cali_ret;
        }
        s_adc_cali_handle = NULL;
        s_adc_cali_enabled = 0u;
    }

    if (s_adc_handle != NULL) {
        int adc_ret = d_battery_err_to_int(adc_oneshot_del_unit(s_adc_handle));
        if (ret == 0 && adc_ret != 0) {
            ret = adc_ret;
        }
        s_adc_handle = NULL;
    }

    return ret;
}

int d_battery_is_initialized(void)
{
    return s_adc_handle != NULL ? 1 : 0;
}

int d_battery_read_voltage_mv(int *voltage_mv)
{
    if (voltage_mv == NULL) {
        return -1;
    }

    int ret = d_battery_init();
    if (ret != 0) {
        return ret;
    }

    ret = d_battery_err_to_int(gpio_set_level(d_battery_ADC_EN_IO, 0));
    if (ret != 0) {
        D_LOGE(TAG, "电池检测使能失败, ret=%d", ret);
        return ret;
    }

    osal_delay_ms(d_battery_ENABLE_SETTLE_MS);

    int raw_sum = 0;
    int raw_min[d_battery_DISCARD_COUNT];
    int raw_max[d_battery_DISCARD_COUNT];
    for (uint32_t i = 0; i < d_battery_DISCARD_COUNT; i++) {
        raw_min[i] = 4095;
        raw_max[i] = 0;
    }

    for (uint32_t i = 0; i < d_battery_SAMPLE_COUNT; i++) {
        int raw = 0;
        ret = d_battery_err_to_int(adc_oneshot_read(s_adc_handle,
                                                      d_battery_ADC_CHANNEL,
                                                      &raw));
        if (ret != 0) {
            (void)gpio_set_level(d_battery_ADC_EN_IO, 1);
            D_LOGE(TAG, "电池 ADC 读取失败, ret=%d", ret);
            return ret;
        }
        raw_sum += raw;

        for (uint32_t j = 0; j < d_battery_DISCARD_COUNT; j++) {
            if (raw < raw_min[j]) {
                for (uint32_t k = d_battery_DISCARD_COUNT - 1u; k > j; k--) {
                    raw_min[k] = raw_min[k - 1u];
                }
                raw_min[j] = raw;
                break;
            }
        }

        for (uint32_t j = 0; j < d_battery_DISCARD_COUNT; j++) {
            if (raw > raw_max[j]) {
                for (uint32_t k = d_battery_DISCARD_COUNT - 1u; k > j; k--) {
                    raw_max[k] = raw_max[k - 1u];
                }
                raw_max[j] = raw;
                break;
            }
        }
    }

    (void)gpio_set_level(d_battery_ADC_EN_IO, 1);

    int discard_sum = 0;
    for (uint32_t i = 0; i < d_battery_DISCARD_COUNT; i++) {
        discard_sum += raw_min[i] + raw_max[i];
    }

    int raw_avg = (raw_sum - discard_sum) /
                  (int)(d_battery_SAMPLE_COUNT - (d_battery_DISCARD_COUNT * 2u));
    int mv = 0;
    if (s_adc_cali_enabled != 0u && s_adc_cali_handle != NULL) {
        ret = d_battery_err_to_int(adc_cali_raw_to_voltage(s_adc_cali_handle, raw_avg, &mv));
        if (ret != 0) {
            D_LOGE(TAG, "电池 ADC 电压转换失败, raw=%d, ret=%d", raw_avg, ret);
            return ret;
        }
    } else {
        mv = (raw_avg * 3100) / 4095;
    }

    *voltage_mv = mv;
    return 0;
}
