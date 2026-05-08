
/**
 * @file osal_task.c
 * @brief 基于 FreeRTOS 的 OSAL 任务与时基接口实现。
 */
#include "osal_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TickType_t osal_task_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OSAL_WAIT_FOREVER) {
        return portMAX_DELAY;
    }

    if (timeout_ms == OSAL_WAIT_NONE) {
        return 0;
    }

    return pdMS_TO_TICKS(timeout_ms);
}

/**
 * @brief 使用 FreeRTOS 当前任务延时实现毫秒延时。
 *
 * @param[in] ms 延时时间，单位为毫秒。
 */
void osal_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/**
 * @brief 获取 FreeRTOS 系统 tick 对应的毫秒计数。
 *
 * @return 系统启动后的毫秒计数。
 */
uint32_t osal_get_tick_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

int osal_task_create(const char *name,
                     osal_task_func_t func,
                     void *arg,
                     uint32_t stack_size,
                     uint32_t priority,
                     osal_task_t *task)
{
    if (func == NULL || stack_size == 0u) {
        return -1;
    }

    TaskHandle_t handle = NULL;
    BaseType_t ret = xTaskCreate(func,
                                 name != NULL ? name : "osal_task",
                                 stack_size,
                                 arg,
                                 priority,
                                 task != NULL ? &handle : NULL);
    if (ret != pdPASS) {
        return -1;
    }

    if (task != NULL) {
        *task = (osal_task_t)handle;
    }
    return 0;
}

int osal_task_notify_give(osal_task_t task)
{
    if (task == NULL) {
        return -1;
    }

    xTaskNotifyGive((TaskHandle_t)task);
    return 0;
}

uint32_t osal_task_notify_take(uint32_t timeout_ms)
{
    return (uint32_t)ulTaskNotifyTake(pdTRUE, osal_task_timeout_to_ticks(timeout_ms));
}
