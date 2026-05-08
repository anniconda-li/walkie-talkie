/**
 * @file osal_mutex.c
 * @brief 基于 FreeRTOS semaphore 实现的 OSAL 互斥锁接口。
 */
#include "osal_mutex.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static TickType_t osal_mutex_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OSAL_WAIT_FOREVER) {
        return portMAX_DELAY;
    }

    if (timeout_ms == OSAL_WAIT_NONE) {
        return 0;
    }

    return pdMS_TO_TICKS(timeout_ms);
}

osal_mutex_t osal_mutex_create(void)
{
    return (osal_mutex_t)xSemaphoreCreateMutex();
}

int osal_mutex_lock(osal_mutex_t mutex, uint32_t timeout_ms)
{
    if (mutex == NULL) {
        return -1;
    }

    return xSemaphoreTake((SemaphoreHandle_t)mutex,
                          osal_mutex_timeout_to_ticks(timeout_ms)) == pdTRUE ? 0 : -1;
}

void osal_mutex_unlock(osal_mutex_t mutex)
{
    if (mutex != NULL) {
        xSemaphoreGive((SemaphoreHandle_t)mutex);
    }
}

void osal_mutex_delete(osal_mutex_t mutex)
{
    if (mutex != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t)mutex);
    }
}
