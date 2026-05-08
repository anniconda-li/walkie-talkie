/**
 * @file osal_mutex.h
 * @brief OSAL 互斥锁抽象接口。
 */
#ifndef OSAL_MUTEX_H
#define OSAL_MUTEX_H

#include <stdint.h>

#ifndef OSAL_WAIT_NONE
#define OSAL_WAIT_NONE     0u
#endif

#ifndef OSAL_WAIT_FOREVER
#define OSAL_WAIT_FOREVER  0xFFFFFFFFu
#endif

/**
 * @brief OSAL 互斥锁句柄。
 */
typedef void *osal_mutex_t;

/**
 * @brief 创建互斥锁。
 *
 * @return 成功返回互斥锁句柄；失败返回 NULL。
 */
osal_mutex_t osal_mutex_create(void);

/**
 * @brief 获取互斥锁。
 *
 * @param[in] mutex 互斥锁句柄。
 * @param[in] timeout_ms 等待超时时间，单位毫秒。
 * @return 成功返回 0；失败返回 -1。
 */
int osal_mutex_lock(osal_mutex_t mutex, uint32_t timeout_ms);

/**
 * @brief 释放互斥锁。
 *
 * @param[in] mutex 互斥锁句柄。
 */
void osal_mutex_unlock(osal_mutex_t mutex);

/**
 * @brief 删除互斥锁。
 *
 * @param[in] mutex 互斥锁句柄。
 */
void osal_mutex_delete(osal_mutex_t mutex);

#endif /* OSAL_MUTEX_H */
