/**
 * @file bsp_max98357a.c
 * @brief MAX98357A I2S 数字功放驱动实现。
 */
#include "bsp_max98357a.h"

#include "bsp_common.h"

#include <stdlib.h>

/**
 * @brief MAX98357A 日志标签。
 */
static const char *TAG = "bsp_max98357a";

/**
 * @brief MAX98357A 驱动对象。
 */
struct max98357a_dev {
    max98357a_interface_t *itf; /**< 底层 I2S 写入接口函数指针。 */
};

max98357a_handle_t max98357a_init(max98357a_interface_t *itf)
{
    if (itf == NULL || itf->write == NULL) {
        BSP_LOGE(TAG, "MAX98357A 初始化失败: 写入接口为空");
        return NULL;
    }

    max98357a_handle_t dev = (max98357a_handle_t)calloc(1, sizeof(struct max98357a_dev));
    if (dev == NULL) {
        BSP_LOGE(TAG, "MAX98357A 初始化失败: 内存分配失败");
        return NULL;
    }

    dev->itf = itf;
    BSP_LOGI(TAG, "MAX98357A 驱动初始化成功");
    return dev;
}

void max98357a_deinit(max98357a_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    free(dev);
    BSP_LOGI(TAG, "MAX98357A 驱动已释放");
}

int max98357a_play(max98357a_handle_t dev,
                   const uint8_t *data,
                   uint32_t len,
                   uint32_t timeout_ms)
{
    if (dev == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "MAX98357A 播放参数无效, dev=%p, data=%p, len=%u",
                 dev, data, (unsigned int)len);
        return -1;
    }

    int ret = dev->itf->write(data, len, timeout_ms);
    if (ret >= 0) {
        BSP_LOGI(TAG, "MAX98357A 播放写入完成, request=%u, written=%d",
                 (unsigned int)len, ret);
    } else {
        BSP_LOGE(TAG, "MAX98357A 播放写入失败, ret=%d", ret);
    }

    return ret;
}
