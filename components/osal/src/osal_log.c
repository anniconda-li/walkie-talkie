/**
 * @file osal_log.c
 * @brief 基于 ESP-IDF log 的 OSAL 日志实现。
 */
#include "osal_log.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>

/**
 * @brief 格式化后日志最大长度。
 */
#define OSAL_LOG_FORMAT_BUF_SIZE 512u

/**
 * @brief 将 OSAL 日志级别转换为 ESP-IDF 日志级别。
 *
 * @param[in] level OSAL 日志级别。
 * @return ESP-IDF 日志级别。
 */
static esp_log_level_t osal_log_to_esp_level(osal_log_level_t level)
{
    switch (level) {
    case OSAL_LOG_LEVEL_ERROR:
        return ESP_LOG_ERROR;
    case OSAL_LOG_LEVEL_WARN:
        return ESP_LOG_WARN;
    case OSAL_LOG_LEVEL_INFO:
        return ESP_LOG_INFO;
    case OSAL_LOG_LEVEL_DEBUG:
        return ESP_LOG_DEBUG;
    default:
        return ESP_LOG_INFO;
    }
}

/**
 * @brief 将 OSAL 日志级别转换为单字符前缀。
 *
 * @param[in] level OSAL 日志级别。
 * @return 日志级别字符。
 */
static char osal_log_level_to_char(osal_log_level_t level)
{
    switch (level) {
    case OSAL_LOG_LEVEL_ERROR:
        return 'E';
    case OSAL_LOG_LEVEL_WARN:
        return 'W';
    case OSAL_LOG_LEVEL_INFO:
        return 'I';
    case OSAL_LOG_LEVEL_DEBUG:
        return 'D';
    default:
        return 'I';
    }
}

void osal_log_write(osal_log_level_t level,
                    const char *tag,
                    const char *fmt,
                    ...)
{
    const char *log_tag = (tag != NULL) ? tag : "";
    const char *log_fmt = (fmt != NULL) ? fmt : "";
    char format_buf[OSAL_LOG_FORMAT_BUF_SIZE];

    (void)snprintf(format_buf,
                   sizeof(format_buf),
                   "%c (%lu) %s: %s\n",
                   osal_log_level_to_char(level),
                   (unsigned long)esp_log_timestamp(),
                   log_tag,
                   log_fmt);

    va_list args;
    va_start(args, fmt);
    esp_log_writev(osal_log_to_esp_level(level), log_tag, format_buf, args);
    va_end(args);
}
