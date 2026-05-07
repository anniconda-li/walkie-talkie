/**
 * @file main.c
 * @brief UI 测试入口。
 */

#include "app_ui.h"
#include "bsp.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_lvgl.h"

/**
 * @brief UI 测试日志标签。
 */
static const char *TAG = "ui_test";

void app_main(void)
{
    OSAL_LOGI(TAG, "开始 UI 测试");

    int ret = bsp_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "BSP 基础资源初始化失败, ret=%d", ret);
        return;
    }

    ret = service_lvgl_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "LVGL 服务初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = app_ui_create();
    if (ret != 0) {
        OSAL_LOGE(TAG, "应用 UI 创建失败, ret=%d", ret);
        (void)service_lvgl_deinit();
        (void)bsp_deinit();
        return;
    }

    OSAL_LOGI(TAG,
              "UI 测试启动完成, res=%ux%u",
              service_lvgl_get_hres(),
              service_lvgl_get_vres());

    while (1) {
        osal_delay_ms(1000);
    }
}
