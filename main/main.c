/**
 * @file main.c
 * @brief 业务应用入口。
 */

#include "app_business.h"
#include "app_ui.h"
#include "bsp.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_screen.h"

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

    ret = service_screen_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "屏幕服务初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = app_ui_create();
    if (ret != 0) {
        OSAL_LOGE(TAG, "应用 UI 创建失败, ret=%d", ret);
        (void)service_screen_deinit();
        (void)bsp_deinit();
        return;
    }

    ret = app_business_start();
    if (ret != 0) {
        OSAL_LOGE(TAG, "业务控制器启动失败, ret=%d", ret);
        (void)service_screen_deinit();
        (void)bsp_deinit();
        return;
    }

    OSAL_LOGI(TAG, "业务应用启动完成");

    while (1) {
        osal_delay_ms(1000u);
    }
}
