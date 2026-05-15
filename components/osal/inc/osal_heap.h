/**
 * @file osal_heap.h
 * @brief OSAL 内存分配抽象接口。
 *
 * 本接口用于隔离 ESP-IDF heap_caps 等平台相关内存能力。上层 app/service
 * 只表达“需要外部大容量内存”这类语义，不直接依赖 PSRAM 或 ESP heap API。
 */
#ifndef OSAL_HEAP_H
#define OSAL_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 分配外部大容量内存。
 *
 * 在 ESP32 平台上优先映射到 PSRAM，适合音频录音缓存、AI WAV 缓存、
 * JPEG 暂存等大块数据。返回内存可按字节访问。
 *
 * @param[in] size 需要分配的字节数。
 * @return 成功返回内存指针；失败返回 NULL。
 */
void *osal_heap_alloc_external(size_t size);

/**
 * @brief 释放通过 OSAL heap 分配的内存。
 *
 * @param[in] ptr 待释放内存；NULL 会被忽略。
 */
void osal_heap_free(void *ptr);

/**
 * @brief 获取当前外部大容量内存剩余字节数。
 *
 * @return 剩余字节数；平台不支持时返回 0。
 */
uint32_t osal_heap_get_external_free_size(void);

/**
 * @brief 获取当前内部内存剩余字节数。
 *
 * 主要用于任务创建失败等日志诊断。
 *
 * @return 剩余字节数；平台不支持时返回 0。
 */
uint32_t osal_heap_get_internal_free_size(void);

/**
 * @brief 获取当前内部内存最大连续空闲块。
 *
 * @return 最大连续空闲块字节数；平台不支持时返回 0。
 */
uint32_t osal_heap_get_internal_largest_free_block(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_HEAP_H */
