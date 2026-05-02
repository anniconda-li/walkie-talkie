/**
 * @file osal_task.h
 * @brief OSAL 任务与系统时基抽象接口。
 */
#ifndef OSAL_TASK_H
#define OSAL_TASK_H

#include <stdint.h>

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

#endif /* OSAL_TASK_H */
