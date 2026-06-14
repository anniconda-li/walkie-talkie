/**
 * @file d_power_control.c
 * @brief 整板电源保持与电源键关机控制实现。
 */
#include "d_power_control.h"

#include "driver/gpio.h"
#include "osal_task.h"

static const char *TAG = "d_power_control";

#define D_POWER_CONTROL_POLL_MS        20u
#define D_POWER_CONTROL_DEBOUNCE_MS    60u
#define D_POWER_CONTROL_LONG_PRESS_MS  2000u
#define D_POWER_CONTROL_TASK_STACK     4096u
#define D_POWER_CONTROL_TASK_PRIORITY  3u

typedef enum {
    D_POWER_CONTROL_STATE_WAIT_BOOT_RELEASE = 0,
    D_POWER_CONTROL_STATE_IDLE,
    D_POWER_CONTROL_STATE_PRESSED,
    D_POWER_CONTROL_STATE_SHUTTING_DOWN,
} d_power_control_state_t;

static volatile int s_initialized = 0;
static volatile int s_monitor_started = 0;
static osal_task_t s_monitor_task = NULL;

static int d_power_control_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

static int d_power_control_key_raw_pressed(void)
{
    int level = gpio_get_level(d_power_control_KEY_IO);
    return level == 0 ? 1 : 0;
}

static int d_power_control_read_debounced_pressed(void)
{
    int first = d_power_control_key_raw_pressed();
    osal_delay_ms(D_POWER_CONTROL_DEBOUNCE_MS);
    int second = d_power_control_key_raw_pressed();

    if (first == second) {
        return second;
    }

    return d_power_control_key_raw_pressed();
}

static void d_power_control_monitor_task(void *arg)
{
    (void)arg;

    d_power_control_state_t state = d_power_control_read_debounced_pressed()
                                        ? D_POWER_CONTROL_STATE_WAIT_BOOT_RELEASE
                                        : D_POWER_CONTROL_STATE_IDLE;
    uint32_t pressed_start_ms = 0u;

    if (state == D_POWER_CONTROL_STATE_WAIT_BOOT_RELEASE) {
        D_LOGI(TAG, "电源键仍处于开机按住状态，等待松开");
    }

    while (1) {
        int pressed = d_power_control_read_debounced_pressed();
        uint32_t now_ms = osal_get_tick_ms();

        switch (state) {
            case D_POWER_CONTROL_STATE_WAIT_BOOT_RELEASE:
                if (!pressed) {
                    D_LOGI(TAG, "开机按键已松开，允许后续长按关机");
                    state = D_POWER_CONTROL_STATE_IDLE;
                }
                break;

            case D_POWER_CONTROL_STATE_IDLE:
                if (pressed) {
                    pressed_start_ms = now_ms;
                    state = D_POWER_CONTROL_STATE_PRESSED;
                }
                break;

            case D_POWER_CONTROL_STATE_PRESSED:
                if (!pressed) {
                    state = D_POWER_CONTROL_STATE_IDLE;
                } else if ((uint32_t)(now_ms - pressed_start_ms) >= D_POWER_CONTROL_LONG_PRESS_MS) {
                    D_LOGI(TAG, "检测到电源键长按，请求关机");
                    (void)d_power_control_shutdown();
                    state = D_POWER_CONTROL_STATE_SHUTTING_DOWN;
                }
                break;

            case D_POWER_CONTROL_STATE_SHUTTING_DOWN:
            default:
                break;
        }

        osal_delay_ms(D_POWER_CONTROL_POLL_MS);
    }
}

int d_power_control_init(void)
{
    if (s_initialized) {
        return 0;
    }

    gpio_config_t hold_cfg = {
        .pin_bit_mask = 1ULL << d_power_control_HOLD_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    int ret = d_power_control_err_to_int(gpio_config(&hold_cfg));
    if (ret != 0) {
        D_LOGE(TAG, "电源保持脚配置失败, io=%d, ret=%d", d_power_control_HOLD_IO, ret);
        return ret;
    }

    ret = d_power_control_err_to_int(gpio_set_level(d_power_control_HOLD_IO, 1));
    if (ret != 0) {
        D_LOGE(TAG, "电源保持脚拉高失败, io=%d, ret=%d", d_power_control_HOLD_IO, ret);
        return ret;
    }

    gpio_config_t key_cfg = {
        .pin_bit_mask = 1ULL << d_power_control_KEY_IO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = d_power_control_err_to_int(gpio_config(&key_cfg));
    if (ret != 0) {
        D_LOGE(TAG, "电源键输入脚配置失败, io=%d, ret=%d", d_power_control_KEY_IO, ret);
        return ret;
    }

    s_initialized = 1;
    D_LOGI(TAG, "电源控制初始化完成, key_io=%d, hold_io=%d",
           d_power_control_KEY_IO,
           d_power_control_HOLD_IO);
    return 0;
}

int d_power_control_start_monitor(void)
{
    int ret = d_power_control_init();
    if (ret != 0) {
        return ret;
    }

    if (s_monitor_started) {
        return 0;
    }

    ret = osal_task_create("power_key",
                           d_power_control_monitor_task,
                           NULL,
                           D_POWER_CONTROL_TASK_STACK,
                           D_POWER_CONTROL_TASK_PRIORITY,
                           &s_monitor_task);
    if (ret != 0) {
        D_LOGE(TAG, "电源键监测任务创建失败, ret=%d", ret);
        return ret;
    }

    s_monitor_started = 1;
    return 0;
}

int d_power_control_shutdown(void)
{
    int ret = d_power_control_init();
    if (ret != 0) {
        return ret;
    }

    ret = d_power_control_err_to_int(gpio_set_level(d_power_control_HOLD_IO, 0));
    if (ret != 0) {
        D_LOGE(TAG, "电源保持脚拉低失败, io=%d, ret=%d", d_power_control_HOLD_IO, ret);
        return ret;
    }

    D_LOGI(TAG, "电源保持脚已拉低，等待硬件断电");
    return 0;
}

int d_power_control_is_key_pressed(void)
{
    int ret = d_power_control_init();
    if (ret != 0) {
        return ret;
    }

    return d_power_control_key_raw_pressed();
}
