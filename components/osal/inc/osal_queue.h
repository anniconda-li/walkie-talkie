/**
 * @file osal_queue.h
 * @brief OSAL 队列抽象接口。
 *
 * 对 FreeRTOS 队列进行轻量封装，使上层代码不直接依赖具体 RTOS 类型。
 */
#ifndef OSAL_QUEUE_H
#define OSAL_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 非阻塞等待时间。
 */
#ifndef OSAL_WAIT_NONE
#define OSAL_WAIT_NONE     0u
#endif

/**
 * @brief 永久阻塞等待时间。
 */
#ifndef OSAL_WAIT_FOREVER
#define OSAL_WAIT_FOREVER  0xFFFFFFFFu
#endif

/**
 * @brief OSAL 队列句柄。
 */
typedef void *osal_queue_t;

/**
 * @brief 创建一个 OSAL 队列。
 *
 * @param[in] item_count 队列可容纳的元素数量。
 * @param[in] item_size 单个元素大小，单位为字节。
 * @return 成功返回队列句柄；失败返回 NULL。
 */
osal_queue_t osal_queue_create(uint32_t item_count, uint32_t item_size);

/**
 * @brief 向队列发送一个元素。
 *
 * @param[in] queue 队列句柄。
 * @param[in] item 待发送元素地址。
 * @param[in] timeout_ms 等待可写空间的超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_send(osal_queue_t queue,
                    const void *item,
                    uint32_t timeout_ms);

/**
 * @brief 从队列接收一个元素。
 *
 * @param[in] queue 队列句柄。
 * @param[out] item 接收元素输出地址。
 * @param[in] timeout_ms 等待队列数据的超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_recv(osal_queue_t queue,
                    void *item,
                    uint32_t timeout_ms);

/**
 * @brief 查看队首元素但不移除。
 *
 * @param[in] queue 队列句柄。
 * @param[out] item 队首元素输出地址。
 * @param[in] timeout_ms 等待队列数据的超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_queue_peek(osal_queue_t queue,
                    void *item,
                    uint32_t timeout_ms);

/**
 * @brief 获取队列中当前等待的元素数量。
 *
 * @param[in] queue 队列句柄。
 * @return 队列中的元素数量；queue 为 NULL 时返回 0。
 */
uint32_t osal_queue_get_count(osal_queue_t queue);

/**
 * @brief 删除队列并释放 RTOS 资源。
 *
 * @param[in] queue 队列句柄。
 */
void osal_queue_delete(osal_queue_t queue);

#endif
