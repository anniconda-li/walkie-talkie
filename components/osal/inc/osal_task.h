/**
 * @file osal_task.h
 * @brief OSAL 任务与系统时基抽象接口。
 */
#ifndef OSAL_TASK_H
#define OSAL_TASK_H

#include <stdint.h>

#ifndef OSAL_WAIT_NONE
#define OSAL_WAIT_NONE     0u
#endif

#ifndef OSAL_WAIT_FOREVER
#define OSAL_WAIT_FOREVER  0xFFFFFFFFu
#endif

/**
 * @brief OSAL 任务句柄。
 */
typedef void *osal_task_t;

/**
 * @brief OSAL 任务入口函数类型。
 */
typedef void (*osal_task_func_t)(void *arg);

/**
 * @brief 当前任务延时指定毫秒数。
 *
 * @param[in] ms 延时时间，单位为毫秒。
 */
void osal_delay_ms(uint32_t ms);

/**
 * @brief 获取系统运行毫秒 tick。
 *
 * @return 系统启动后的毫秒计数。
 */
uint32_t osal_get_tick_ms(void);

/**
 * @brief 创建任务。
 *
 * @param[in] name 任务名。
 * @param[in] func 任务入口。
 * @param[in] arg 任务参数。
 * @param[in] stack_size 栈大小，单位字节。
 * @param[in] priority 任务优先级。
 * @param[out] task 任务句柄输出，可为 NULL。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_task_create(const char *name,
                     osal_task_func_t func,
                     void *arg,
                     uint32_t stack_size,
                     uint32_t priority,
                     osal_task_t *task);

/**
 * @brief 删除当前任务。
 *
 * 一次性后台任务完成工作后调用本接口退出。
 */
void osal_task_delete_current(void);

/**
 * @brief 向任务发送通知。
 *
 * @param[in] task 任务句柄。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_task_notify_give(osal_task_t task);

/**
 * @brief 等待并清除任务通知。
 *
 * @param[in] timeout_ms 等待超时时间，单位毫秒。
 * @return 收到的通知计数；超时返回 0。
 */
uint32_t osal_task_notify_take(uint32_t timeout_ms);

#endif /* OSAL_TASK_H */
