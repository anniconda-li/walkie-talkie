
/**
 * @file osal_task.c
 * @brief 基于 FreeRTOS 的 OSAL 任务与时基接口实现。
 */
#include "osal_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
