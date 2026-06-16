/**
 * @file service_buttons.c
 * @brief Debounced polling service for PCA-backed board buttons.
 */
#include "service_buttons.h"

#include "osal_task.h"
#include "service_config.h"

#include <stddef.h>

static const char *TAG = "service_buttons";

#define SERVICE_BUTTONS_TASK_STACK       3072u
#define SERVICE_BUTTONS_TASK_PRIORITY    3u
#define SERVICE_BUTTONS_POLL_MS          20u
#define SERVICE_BUTTONS_DEBOUNCE_COUNT   3u
#define SERVICE_BUTTONS_LONG_PRESS_MS    500u
#define SERVICE_BUTTONS_REPEAT_MS        120u

typedef struct {
    uint8_t stable_pressed;
    uint8_t last_raw_pressed;
    uint8_t debounce_count;
    uint8_t long_active;
    uint32_t pressed_ms;
    uint32_t last_repeat_ms;
} service_buttons_key_state_t;

static service_buttons_config_t s_buttons_cfg;
static service_buttons_callbacks_t s_buttons_callbacks;
static osal_task_t s_buttons_task = NULL;
static uint8_t s_buttons_inited = 0u;
static uint8_t s_buttons_started = 0u;

static int service_buttons_read_pressed(int up, uint8_t *pressed)
{
    if (pressed == NULL) {
        return -1;
    }

    service_buttons_level_t level = SERVICE_BUTTONS_LEVEL_HIGH;
    int ret = up ? s_buttons_cfg.read_volume_up(&level) :
                   s_buttons_cfg.read_volume_down(&level);
    if (ret != 0) {
        return ret;
    }

    *pressed = (level == s_buttons_cfg.active_level) ? 1u : 0u;
    return 0;
}

static void service_buttons_emit_step(int up)
{
    if (s_buttons_callbacks.volume_step != NULL) {
        s_buttons_callbacks.volume_step(up ? 10 : -10);
    }
}

static void service_buttons_update_key(service_buttons_key_state_t *state,
                                       int up,
                                       uint32_t now_ms)
{
    uint8_t raw_pressed = 0u;
    if (service_buttons_read_pressed(up, &raw_pressed) != 0) {
        return;
    }

    if (raw_pressed == state->last_raw_pressed) {
        if (state->debounce_count < SERVICE_BUTTONS_DEBOUNCE_COUNT) {
            state->debounce_count++;
        }
    } else {
        state->last_raw_pressed = raw_pressed;
        state->debounce_count = 1u;
    }

    if (state->debounce_count < SERVICE_BUTTONS_DEBOUNCE_COUNT ||
        raw_pressed == state->stable_pressed) {
        if (state->stable_pressed != 0u) {
            if (state->long_active == 0u &&
                (uint32_t)(now_ms - state->pressed_ms) >= SERVICE_BUTTONS_LONG_PRESS_MS) {
                state->long_active = 1u;
                state->last_repeat_ms = now_ms;
                service_buttons_emit_step(up);
            } else if (state->long_active != 0u &&
                       (uint32_t)(now_ms - state->last_repeat_ms) >= SERVICE_BUTTONS_REPEAT_MS) {
                state->last_repeat_ms = now_ms;
                service_buttons_emit_step(up);
            }
        }
        return;
    }

    state->stable_pressed = raw_pressed;
    state->long_active = 0u;
    state->pressed_ms = now_ms;
    state->last_repeat_ms = now_ms;

    if (raw_pressed != 0u) {
        service_buttons_emit_step(up);
    }
}

static void service_buttons_task(void *arg)
{
    (void)arg;
    service_buttons_key_state_t up_state = {0};
    service_buttons_key_state_t down_state = {0};

    while (1) {
        uint32_t now_ms = osal_get_tick_ms();
        service_buttons_update_key(&up_state, 1, now_ms);
        service_buttons_update_key(&down_state, 0, now_ms);
        osal_delay_ms(SERVICE_BUTTONS_POLL_MS);
    }
}

int service_buttons_init(const service_buttons_config_t *cfg)
{
    if (s_buttons_inited != 0u) {
        return 0;
    }
    if (cfg == NULL ||
        cfg->read_volume_up == NULL ||
        cfg->read_volume_down == NULL) {
        SERVICE_LOGE(TAG, "按键服务初始化失败: ops 无效");
        return -1;
    }

    s_buttons_cfg = *cfg;
    s_buttons_inited = 1u;
    SERVICE_LOGI(TAG, "按键服务初始化成功, active_level=%d", s_buttons_cfg.active_level);
    return 0;
}

int service_buttons_deinit(void)
{
    s_buttons_inited = 0u;
    s_buttons_started = 0u;
    s_buttons_task = NULL;
    s_buttons_cfg = (service_buttons_config_t){0};
    s_buttons_callbacks = (service_buttons_callbacks_t){0};
    return 0;
}

int service_buttons_start(const service_buttons_callbacks_t *callbacks)
{
    if (s_buttons_inited == 0u) {
        SERVICE_LOGE(TAG, "按键服务启动失败: 未初始化");
        return -1;
    }
    if (callbacks == NULL || callbacks->volume_step == NULL) {
        SERVICE_LOGE(TAG, "按键服务启动失败: callbacks 无效");
        return -2;
    }

    s_buttons_callbacks = *callbacks;
    if (s_buttons_started != 0u) {
        return 0;
    }

    int ret = osal_task_create("svc_buttons",
                               service_buttons_task,
                               NULL,
                               SERVICE_BUTTONS_TASK_STACK,
                               SERVICE_BUTTONS_TASK_PRIORITY,
                               &s_buttons_task);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "按键服务任务创建失败, ret=%d", ret);
        return ret;
    }

    s_buttons_started = 1u;
    SERVICE_LOGI(TAG, "按键服务已启动");
    return 0;
}
