/**
 * @file service_config.h
 * @brief Service 层公共配置和日志宏。
 */
#ifndef SERVICE_CONFIG_H
#define SERVICE_CONFIG_H

#include "osal_log.h"

/** @brief Service 调试模式开关。 */
#define SERVICE_DEBUG 1

#if SERVICE_DEBUG
/** @brief Service 信息日志宏。 */
#define SERVICE_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)
/** @brief Service 警告日志宏。 */
#define SERVICE_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)
/** @brief Service 错误日志宏。 */
#define SERVICE_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define SERVICE_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define SERVICE_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define SERVICE_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

#endif /* SERVICE_CONFIG_H */
