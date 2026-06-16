/**
 * @file main.c
 * @brief Firmware business entry.
 */

#include "app_boot_status.h"
#include "app_business.h"
#include "app_ui.h"
#include "d_config.h"
#include "d_init.h"
#include "d_power_control.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_init.h"
#include "service_screen.h"
#include "ui_splash.h"
#include "wdriver.h"

static const char *TAG = "main";

static void main_fatal(const char *stage, int ret)
{
    OSAL_LOGE(TAG, "%s失败, ret=%d", stage, ret);
    while (1) {
        osal_delay_ms(1000u);
    }
}

static int main_set_stage_running(app_boot_stage_t stage)
{
    app_boot_status_set(stage, APP_BOOT_STATE_RUNNING, 0);
    OSAL_LOGI(TAG, "启动阶段开始: %s", app_boot_status_stage_name(stage));
    return 0;
}

static int main_finish_stage(app_boot_stage_t stage, int ret, int fatal)
{
    if (ret == 0) {
        app_boot_status_set(stage, APP_BOOT_STATE_OK, 0);
        OSAL_LOGI(TAG, "启动阶段完成: %s", app_boot_status_stage_name(stage));
        return 0;
    }

    app_boot_status_set(stage, fatal ? APP_BOOT_STATE_ERROR : APP_BOOT_STATE_WARN, ret);
    if (fatal) {
        OSAL_LOGE(TAG, "启动阶段失败: %s, ret=%d", app_boot_status_stage_name(stage), ret);
    } else {
        OSAL_LOGW(TAG, "启动阶段警告: %s, ret=%d", app_boot_status_stage_name(stage), ret);
    }
    return ret;
}

void app_main(void)
{
    OSAL_LOGI(TAG, "业务固件启动开始");

    int ret = d_power_control_init();
    if (ret != 0) {
        main_fatal("电源保持初始化", ret);
    }

    ret = d_power_control_start_monitor();
    if (ret != 0) {
        main_fatal("电源键监测任务启动", ret);
    }

    app_boot_status_reset();

    (void)main_set_stage_running(APP_BOOT_STAGE_WDRIVER);
    ret = wdriver_init();
    if (main_finish_stage(APP_BOOT_STAGE_WDRIVER, ret, 1) != 0) {
        main_fatal("WDRIVER 基础资源初始化", ret);
    }

    (void)main_set_stage_running(APP_BOOT_STAGE_SCREEN);
    ret = d_screen_init();
    if (ret == 0) {
        ret = service_init_screen();
        if (ret == 0) {
            ret = service_init_buttons();
        }
    }
    if (main_finish_stage(APP_BOOT_STAGE_SCREEN, ret, 1) != 0) {
        main_fatal("屏幕初始化", ret);
    }

    (void)main_set_stage_running(APP_BOOT_STAGE_AUDIO);
    ret = d_audio_init();
    if (ret == 0) {
        ret = service_init_audio();
    }
    if (main_finish_stage(APP_BOOT_STAGE_AUDIO, ret, 1) != 0) {
        main_fatal("ES 音频初始化", ret);
    }

    (void)main_set_stage_running(APP_BOOT_STAGE_BATTERY);
    ret = d_power_init();
    if (ret == 0) {
        ret = service_init_battery();
    }
    if (main_finish_stage(APP_BOOT_STAGE_BATTERY, ret, 1) != 0) {
        main_fatal("电池服务初始化", ret);
    }

    (void)main_set_stage_running(APP_BOOT_STAGE_NETWORK);
    app_boot_status_set(APP_BOOT_STAGE_NETWORK, APP_BOOT_STATE_OK, 0);
    OSAL_LOGI(TAG, "网络阶段由运行期业务启动");

    (void)main_set_stage_running(APP_BOOT_STAGE_CAMERA);
#if D_INIT_ENABLE_CAMERA
    ret = d_optional_camera_init();
    if (ret == 0) {
        ret = service_init_camera();
    }
    (void)main_finish_stage(APP_BOOT_STAGE_CAMERA, ret, 0);
#else
    (void)main_finish_stage(APP_BOOT_STAGE_CAMERA, -1, 0);
#endif

    (void)main_set_stage_running(APP_BOOT_STAGE_UI);
    ret = app_ui_create();
    if (main_finish_stage(APP_BOOT_STAGE_UI, ret, 1) != 0) {
        main_fatal("应用 UI 创建", ret);
    }

    (void)main_set_stage_running(APP_BOOT_STAGE_RUNTIME);
    ret = app_business_start();
    if (main_finish_stage(APP_BOOT_STAGE_RUNTIME, ret, 1) != 0) {
        main_fatal("业务任务启动", ret);
    }

    if (service_screen_lock(1000u) == 0) {
        ui_splash_finish();
        service_screen_unlock();
    } else {
        OSAL_LOGW(TAG, "启动页结束失败: LVGL 加锁超时");
    }

    OSAL_LOGI(TAG, "业务固件启动完成");

    while (1) {
        osal_delay_ms(60000u);
    }
}
