/**
 * @file bsp_inmp441.c
 * @brief INMP441 I2S 数字麦克风驱动实现。
 */
#include "bsp_inmp441.h"

#include "bsp_common.h"

#include <stdlib.h>

/**
 * @brief INMP441 日志标签。
 */
static const char *TAG = "bsp_inmp441";

/**
 * @brief INMP441 驱动对象。
 */
struct inmp441_dev {
    inmp441_interface_t *itf; /**< 底层 I2S 读取接口函数指针。 */
};

inmp441_handle_t inmp441_init(inmp441_interface_t *itf)
{
    if (itf == NULL || itf->read == NULL) {
        BSP_LOGE(TAG, "INMP441 初始化失败: 读取接口为空");
        return NULL;
    }

    inmp441_handle_t dev = (inmp441_handle_t)calloc(1, sizeof(struct inmp441_dev));
    if (dev == NULL) {
        BSP_LOGE(TAG, "INMP441 初始化失败: 内存分配失败");
        return NULL;
    }

    dev->itf = itf;
    BSP_LOGI(TAG, "INMP441 驱动初始化成功");
    return dev;
}

void inmp441_deinit(inmp441_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    free(dev);
    BSP_LOGI(TAG, "INMP441 驱动已释放");
}

int inmp441_read(inmp441_handle_t dev,
                 uint8_t *data,
                 uint32_t len,
                 uint32_t timeout_ms)
{
    if (dev == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "INMP441 读取参数无效, dev=%p, data=%p, len=%u",
                 dev, data, (unsigned int)len);
        return -1;
    }

    int ret = dev->itf->read(data, len, timeout_ms);
    if (ret >= 0) {
        BSP_LOGI(TAG, "INMP441 读取完成, request=%u, read=%d",
                 (unsigned int)len, ret);
    } else {
        BSP_LOGE(TAG, "INMP441 读取失败, ret=%d", ret);
    }

    return ret;
}
