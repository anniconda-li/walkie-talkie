/**
 * @file app_common.h
 * @brief App 层公共配置。
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include "osal_log.h"

/**
 * @brief App 调试模式开关。
 *
 * 定义为 1 时开启调试日志，定义为 0 时关闭调试日志。
 */
#define APP_DEBUG 1  /**< 默认开启，发布时改为 0。 */

#if APP_DEBUG
/**
 * @brief App 信息日志宏。
 */
#define APP_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)

/**
 * @brief App 警告日志宏。
 */
#define APP_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)

/**
 * @brief App 错误日志宏。
 */
#define APP_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
/**
 * @brief App 信息日志空实现。
 */
#define APP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)

/**
 * @brief App 警告日志空实现。
 */
#define APP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)

/**
 * @brief App 错误日志空实现。
 */
#define APP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

#endif /* APP_COMMON_H */
