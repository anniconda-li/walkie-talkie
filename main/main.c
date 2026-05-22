/**
 * @file main.c
 * @brief 业务应用入口。
 */

#include "app_business.h"
#include "app_boot_status.h"
#include "app_ui.h"
#include "bsp.h"
#include "driver_init.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_init.h"
#include "service_network.h"
#include "service_screen.h"
#include "ui_splash.h"
#include <stdlib.h>

/**
 * @brief 应用日志标签。
 */
static const char *TAG = "walkie_app";

static int s_boot_ui_visible = 0;

static const char *network_reg_desc(int reg_state)
{
    switch (reg_state) {
    case 0:
        return "未注册";
    case 1:
        return "已注册";
    case 2:
        return "正在搜索";
    case 3:
        return "注册被拒";
    case 4:
        return "未知";
    case 5:
        return "漫游注册";
    default:
        return "查询失败";
    }
}

static const char *network_link_desc(int link_state)
{
    if (link_state == 1) {
        return "已连接";
    }
    if (link_state == 0) {
        return "未连接";
    }
    return "查询失败";
}

static const char *network_not_ready_reason(const service_network_status_t *status)
{
    if (status == NULL) {
        return "状态为空";
    }
    if (status->at_ready != 1) {
        return "AT 通信失败";
    }
    if (status->sim_ready != 1) {
        return "SIM/ICCID 未就绪";
    }
    if (status->reg_state != 1 && status->reg_state != 5) {
        return "蜂窝网络未注册";
    }
    if (status->link_state != 1) {
        return "数据链路未连接";
    }
    return "未知";
}

static void boot_refresh_ui(void)
{
    if (!s_boot_ui_visible) {
        return;
    }

    if (service_screen_lock(100u) == 0) {
        ui_splash_refresh_status();
        service_screen_unlock();
    }
}

static void boot_mark(app_boot_stage_t stage, app_boot_state_t state, int code)
{
    app_boot_status_set(stage, state, code);
    boot_refresh_ui();
}

static void boot_fatal(app_boot_stage_t stage, int code)
{
    const char *stage_name = app_boot_status_stage_name(stage);

    boot_mark(stage, APP_BOOT_STATE_ERROR, code);
    OSAL_LOGE(TAG, "启动失败: stage=%s, ret=%d", stage_name, code);

    if (s_boot_ui_visible && service_screen_lock(100u) == 0) {
        ui_splash_show_error(stage_name, code);
        service_screen_unlock();
    }

    while (1) {
        osal_delay_ms(1000u);
    }
}

void app_main(void)
{
    OSAL_LOGI(TAG, "开始临时 4G/ML307C 测试启动");

    int ret = bsp_init();

    if (ret != 0) {
        OSAL_LOGE(TAG, "4G 测试失败: BSP 初始化失败, ret=%d", ret);
        while (1) {
            osal_delay_ms(1000u);
        }
    }

    ret = driver_network_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "4G 测试失败: ML307C driver 初始化失败, ret=%d", ret);
        while (1) {
            osal_delay_ms(1000u);
        }
    }

    ret = service_init_network();
    if (ret != 0) {
        OSAL_LOGE(TAG, "4G 测试失败: network service 初始化失败, ret=%d", ret);
        while (1) {
            osal_delay_ms(1000u);
        }
    }

    OSAL_LOGI(TAG, "4G 测试初始化完成，开始周期打印网络状态");

    service_network_status_t last_status = {
        .rssi = -999,
        .reg_state = -999,
        .link_state = -999,
        .sim_ready = -999,
        .at_ready = -999,
    };
    int last_ready = -999;
    uint32_t loop_count = 0u;

    while (1) {
        service_network_status_t status = {0};
        int status_ret = service_network_get_status(&status);
        int ready = status.at_ready == 1 &&
                    status.sim_ready == 1 &&
                    (status.reg_state == 1 || status.reg_state == 5) &&
                    status.link_state == 1;

        if (status_ret == 0) {
            int changed = ready != last_ready ||
                          status.at_ready != last_status.at_ready ||
                          status.sim_ready != last_status.sim_ready ||
                          status.reg_state != last_status.reg_state ||
                          status.link_state != last_status.link_state ||
                          status.rssi != last_status.rssi;

            if (changed || (loop_count % 4u) == 0u) {
                OSAL_LOGI(TAG,
                          "4G 状态: %s, 原因=%s, AT=%s, SIM=%s, 注册=%s(%d), 链路=%s(%d), RSSI=%d",
                          ready == 1 ? "可用" : "不可用",
                          ready == 1 ? "正常" : network_not_ready_reason(&status),
                          status.at_ready == 1 ? "正常" : "失败",
                          status.sim_ready == 1 ? "正常" : "失败",
                          network_reg_desc(status.reg_state),
                          status.reg_state,
                          network_link_desc(status.link_state),
                          status.link_state,
                          status.rssi);

                last_status = status;
                last_ready = ready;
            }
        } else {
            OSAL_LOGW(TAG, "4G 状态读取失败, ret=%d, ready=%d", status_ret, ready);
        }

        loop_count++;
        osal_delay_ms(5000u);
    }

#if 0
    OSAL_LOGI(TAG, "开始启动业务应用");

    app_boot_status_reset();

    boot_mark(APP_BOOT_STAGE_BSP, APP_BOOT_STATE_RUNNING, 0);
    int ret = bsp_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "BSP 基础资源初始化失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_BSP, ret);
    }
    boot_mark(APP_BOOT_STAGE_BSP, APP_BOOT_STATE_OK, 0);

    boot_mark(APP_BOOT_STAGE_SCREEN, APP_BOOT_STATE_RUNNING, 0);
    ret = driver_screen_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "屏幕 driver 初始化失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_SCREEN, ret);
    }

    ret = service_init_screen();
    if (ret != 0) {
        OSAL_LOGE(TAG, "屏幕 service 初始化失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_SCREEN, ret);
    }

    ret = app_ui_create();
    if (ret != 0) {
        OSAL_LOGE(TAG, "启动 UI 创建失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_UI, ret);
    }
    s_boot_ui_visible = 1;
    boot_mark(APP_BOOT_STAGE_SCREEN, APP_BOOT_STATE_OK, 0);
    boot_mark(APP_BOOT_STAGE_UI, APP_BOOT_STATE_OK, 0);

    boot_mark(APP_BOOT_STAGE_AUDIO, APP_BOOT_STATE_RUNNING, 0);
    ret = driver_audio_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "音频 driver 初始化失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_AUDIO, ret);
    }
    ret = service_init_audio();
    if (ret != 0) {
        OSAL_LOGE(TAG, "音频 service 初始化失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_AUDIO, ret);
    }
    boot_mark(APP_BOOT_STAGE_AUDIO, APP_BOOT_STATE_OK, 0);

    boot_mark(APP_BOOT_STAGE_BATTERY, APP_BOOT_STATE_RUNNING, 0);
    ret = driver_power_init();
    if (ret != 0) {
        OSAL_LOGW(TAG, "电池 driver 初始化失败，电量显示将不可用, ret=%d", ret);
        boot_mark(APP_BOOT_STAGE_BATTERY, APP_BOOT_STATE_WARN, ret);
    } else {
        ret = service_init_battery();
        if (ret != 0) {
            OSAL_LOGW(TAG, "电池 service 初始化失败，电量显示将不可用, ret=%d", ret);
            boot_mark(APP_BOOT_STAGE_BATTERY, APP_BOOT_STATE_WARN, ret);
        } else {
            boot_mark(APP_BOOT_STAGE_BATTERY, APP_BOOT_STATE_OK, 0);
        }
    }

    boot_mark(APP_BOOT_STAGE_NETWORK, APP_BOOT_STATE_RUNNING, 0);
    ret = driver_network_init();
    if (ret != 0) {
        OSAL_LOGW(TAG, "网络 driver 初始化失败，业务将以未联网状态继续, ret=%d", ret);
    }
    int network_service_ret = service_init_network();
    if (ret != 0 || network_service_ret != 0) {
        boot_mark(APP_BOOT_STAGE_NETWORK,
                  APP_BOOT_STATE_WARN,
                  ret != 0 ? ret : network_service_ret);
    } else {
        boot_mark(APP_BOOT_STAGE_NETWORK, APP_BOOT_STATE_OK, 0);
    }

    boot_mark(APP_BOOT_STAGE_CAMERA, APP_BOOT_STATE_RUNNING, 0);
    ret = driver_optional_camera_init();
    if (ret != 0) {
        boot_mark(APP_BOOT_STAGE_CAMERA, APP_BOOT_STATE_WARN, ret);
    } else {
        ret = service_init_camera();
        if (ret != 0) {
            boot_mark(APP_BOOT_STAGE_CAMERA, APP_BOOT_STATE_WARN, ret);
        } else {
            boot_mark(APP_BOOT_STAGE_CAMERA, APP_BOOT_STATE_OK, 0);
        }
    }

    boot_mark(APP_BOOT_STAGE_RUNTIME, APP_BOOT_STATE_RUNNING, 0);
    ret = app_business_start_runtime();
    if (ret != 0) {
        OSAL_LOGE(TAG, "业务运行期启动失败, ret=%d", ret);
        boot_fatal(APP_BOOT_STAGE_RUNTIME, ret);
    }
    boot_mark(APP_BOOT_STAGE_RUNTIME, APP_BOOT_STATE_OK, 0);

    if (service_screen_lock(100u) == 0) {
        ui_splash_finish();
        service_screen_unlock();
    }

    OSAL_LOGI(TAG, "业务应用启动完成");

    while (1) {
        osal_delay_ms(1000u);
    }
#endif
}
