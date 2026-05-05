/**
 * @file osal_log.h
 * @brief OSAL 日志抽象接口。
 */
#ifndef OSAL_LOG_H
#define OSAL_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通用日志级别。
 */
typedef enum {
    OSAL_LOG_LEVEL_ERROR = 0,
    OSAL_LOG_LEVEL_WARN,
    OSAL_LOG_LEVEL_INFO,
    OSAL_LOG_LEVEL_DEBUG,
} osal_log_level_t;

/**
 * @brief 输出一条日志。
 *
 * @param[in] level 日志级别。
 * @param[in] tag 日志标签。
 * @param[in] fmt printf 风格格式字符串。
 */
void osal_log_write(osal_log_level_t level,
                    const char *tag,
                    const char *fmt,
                    ...);

/**
 * @brief OSAL 错误日志宏。
 */
#define OSAL_LOGE(tag, fmt, ...) osal_log_write(OSAL_LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

/**
 * @brief OSAL 警告日志宏。
 */
#define OSAL_LOGW(tag, fmt, ...) osal_log_write(OSAL_LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)

/**
 * @brief OSAL 信息日志宏。
 */
#define OSAL_LOGI(tag, fmt, ...) osal_log_write(OSAL_LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)

/**
 * @brief OSAL 调试日志宏。
 */
#define OSAL_LOGD(tag, fmt, ...) osal_log_write(OSAL_LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* OSAL_LOG_H */
