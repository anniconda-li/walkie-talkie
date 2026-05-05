/**
 * @file service_common.h
 * @brief Service 层公共配置。
 *
 * 本文件集中放置 service 组件共用的编译期开关，便于不同功能服务保持一致的配置入口。
 */
#ifndef SERVICE_COMMON_H
#define SERVICE_COMMON_H

#include "osal_log.h"

/**
 * @brief Service 调试模式开关。
 *
 * 定义为 1 时开启调试日志，定义为 0 时关闭调试日志。
 */
#define SERVICE_DEBUG 1  // 默认开启，发布时改为 0

#if SERVICE_DEBUG
/**
 * @brief Service 信息日志宏。
 */
#define SERVICE_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)

/**
 * @brief Service 警告日志宏。
 */
#define SERVICE_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)

/**
 * @brief Service 错误日志宏。
 */
#define SERVICE_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define SERVICE_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define SERVICE_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define SERVICE_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

#endif /* SERVICE_COMMON_H */
