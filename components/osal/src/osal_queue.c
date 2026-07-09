/**
 * @file osal_queue.c
 * @brief 基于 FreeRTOS 队列实现的 OSAL 队列接口。
 */

#include "osal_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief 将 OSAL 毫秒超时值转换为 FreeRTOS tick。
 *
 * @param[in] timeout_ms OSAL 超时时间，单位为毫秒。
 * @return FreeRTOS 队列 API 使用的 tick 数。
 */
static TickType_t osal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OSAL_WAIT_FOREVER) {
        return portMAX_DELAY;
    }

    if (timeout_ms == OSAL_WAIT_NONE) {
        return 0;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks > 0 ? ticks : 1;
}

/**
 * @brief 创建一个 FreeRTOS 队列。
 *
 * @param[in] item_count 队列元素数量。
 * @param[in] item_size 单个元素大小，单位为字节。
 * @return 成功返回 OSAL 队列句柄；失败返回 NULL。
 */
osal_queue_t osal_queue_create(uint32_t item_count, uint32_t item_size)
{
    if (item_count == 0 || item_size == 0) {
        return NULL;
    }

    return (osal_queue_t)xQueueCreate(item_count, item_size);
}

/**
 * @brief 向 FreeRTOS 队列发送元素。
 *
 * @param[in] queue 队列句柄。
 * @param[in] item 待发送元素地址。
 * @param[in] timeout_ms 等待超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_send(osal_queue_t queue,
                    const void *item,
                    uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return -1;
    }

    BaseType_t ret = xQueueSend((QueueHandle_t)queue,
                                item,
                                osal_timeout_to_ticks(timeout_ms));

    return ret == pdTRUE ? 0 : -1;
}

/**
 * @brief 从 FreeRTOS 队列接收元素。
 *
 * @param[in] queue 队列句柄。
 * @param[out] item 接收元素输出地址。
 * @param[in] timeout_ms 等待超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_recv(osal_queue_t queue,
                    void *item,
                    uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return -1;
    }

    BaseType_t ret = xQueueReceive((QueueHandle_t)queue,
                                   item,
                                   osal_timeout_to_ticks(timeout_ms));

    return ret == pdTRUE ? 0 : -1;
}

/**
 * @brief 读取 FreeRTOS 队首元素但不移除。
 *
 * @param[in] queue 队列句柄。
 * @param[out] item 队首元素输出地址。
 * @param[in] timeout_ms 等待超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_peek(osal_queue_t queue,
                    void *item,
                    uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return -1;
    }

    BaseType_t ret = xQueuePeek((QueueHandle_t)queue,
                                item,
                                osal_timeout_to_ticks(timeout_ms));

    return ret == pdTRUE ? 0 : -1;
}

/**
 * @brief 获取 FreeRTOS 队列中等待的元素数量。
 *
 * @param[in] queue 队列句柄。
 * @return 队列元素数量；queue 为 NULL 时返回 0。
 */
uint32_t osal_queue_get_count(osal_queue_t queue)
{
    if (queue == NULL) {
        return 0;
    }

    return (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)queue);
}

/**
 * @brief 删除 FreeRTOS 队列。
 *
 * @param[in] queue 队列句柄。
 */
void osal_queue_delete(osal_queue_t queue)
{
    if (queue != NULL) {
        vQueueDelete((QueueHandle_t)queue);
    }
}
