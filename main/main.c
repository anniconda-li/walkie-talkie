/**
 * @file main.c
 * @brief 业务应用入口。
 */

#include "app_business.h"
#include "bsp.h"
#include "driver_init.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_init.h"

/**
 * @brief 应用日志标签。
 */
static const char *TAG = "walkie_app";

void app_main(void)
{
    OSAL_LOGI(TAG, "开始启动业务应用");

    int ret = bsp_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "BSP 基础资源初始化失败, ret=%d", ret);
        return;
    }

    driver_init_config_t driver_cfg = service_init_get_driver_config();
    ret = driver_init(&driver_cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "Driver 初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = service_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "Service 初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = app_business_start();
    if (ret != 0) {
        OSAL_LOGE(TAG, "业务控制器启动失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    OSAL_LOGI(TAG, "业务应用启动完成");

    while (1) {
        osal_delay_ms(1000u);
    }
}
