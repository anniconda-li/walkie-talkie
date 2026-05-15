/**
 * @file osal_heap.c
 * @brief 基于 ESP-IDF heap_caps 的 OSAL 内存实现。
 */
#include "osal_heap.h"

#include "esp_heap_caps.h"

void *osal_heap_alloc_external(size_t size)
{
    if (size == 0u) {
        return NULL;
    }

    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void osal_heap_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

uint32_t osal_heap_get_external_free_size(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t osal_heap_get_internal_free_size(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t osal_heap_get_internal_largest_free_block(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}
