/**
 * @file main.c
 * @brief LVGL 显示与触摸基础测试入口。
 */

#include "app_ui.h"
#include "bsp_i2c.h"
#include "bsp_pca9557.h"
#include "osal_log.h"
#include "osal_task.h"
#include "service_lvgl.h"

/**
 * @brief LVGL 测试日志标签。
 */
static const char *TAG = "lvgl_test";

/**
 * @brief LCD 背光与摄像头电源控制输出值。
 *
 * bit5 为 LCD_BL，输出 1 打开背光；bit1 为 OV-PWDN，输出 0 唤醒摄像头。
 */
#define LVGL_TEST_PCA9557_OUTPUT_INIT 0x20u

/**
 * @brief LVGL 测试使用的 PCA9557 方向值。
 *
 * PCA9557 方向寄存器 1 表示输入，0 表示输出；此处 IO1 和 IO5 为输出，其余为输入。
 */
#define LVGL_TEST_PCA9557_DIRECTION_INIT 0xDDu

/**
 * @brief LVGL 测试持有的 PCA9557 句柄。
 */
static pca9557_handle_t s_pca9557 = NULL;

/**
 * @brief 打印通用测试步骤结果。
 *
 * @param[in] name 测试步骤名称。
 * @param[in] ret 测试步骤返回值。
 * @return 原样返回 ret，便于调用方继续判断。
 */
static int log_step_result(const char *name, int ret)
{
    if (ret == 0) {
        OSAL_LOGI(TAG, "%s: 成功", name);
    } else {
        OSAL_LOGE(TAG, "%s: 失败, ret=%d", name, ret);
    }

    return ret;
}

/**
 * @brief 准备 LVGL 测试需要的板级 IO 状态。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int board_io_prepare_for_lvgl(void)
{
    if (log_step_result("I2C init", bsp_i2c_init()) != 0) {
        return -1;
    }

    pca9557_interface_t pca9557_itf = {
        .write_reg = pca9557_i2c_write_reg_impl,
        .read_reg = pca9557_i2c_read_reg_impl,
    };

    pca9557_config_t pca9557_cfg = {
        .output_init = LVGL_TEST_PCA9557_OUTPUT_INIT,
        .polarity_init = 0x00,
        .direction_init = LVGL_TEST_PCA9557_DIRECTION_INIT,
    };

    s_pca9557 = pca9557_init(&pca9557_cfg, &pca9557_itf);
    if (s_pca9557 == NULL) {
        OSAL_LOGE(TAG, "PCA9557 初始化失败");
        return -2;
    }

    OSAL_LOGI(TAG,
              "PCA9557 已配置: output=0x%02X, direction=0x%02X, LCD_BL=1, OV-PWDN=0",
              (unsigned int)LVGL_TEST_PCA9557_OUTPUT_INIT,
              (unsigned int)LVGL_TEST_PCA9557_DIRECTION_INIT);
    return 0;
}

/**
 * @brief 循环更新 UI 状态，验证 LVGL 刷新。
 */
static void lvgl_ui_state_test_loop(void)
{
    int state = 0;

    while (1) {
        app_ui_set_network_state(state % 4);
        app_ui_set_intercom_state((state / 2) % 2);
        app_ui_set_record_state((state / 3) % 2);

        OSAL_LOGI(TAG, "LVGL UI 状态更新, state=%d", state);
        state++;
        osal_delay_ms(1000);
    }
}

/**
 * @brief ESP-IDF 应用入口。
 */
void app_main(void)
{
    OSAL_LOGI(TAG, "开始 LVGL 测试");

    if (log_step_result("Board IO prepare", board_io_prepare_for_lvgl()) != 0) {
        return;
    }

    if (log_step_result("LVGL service init", service_lvgl_init()) != 0) {
        return;
    }

    if (log_step_result("App UI create", app_ui_create()) != 0) {
        return;
    }

    OSAL_LOGI(TAG, "LVGL 测试界面已创建，等待启动动画完成");
    osal_delay_ms(1200);

    OSAL_LOGI(TAG, "开始循环更新 LVGL 测试状态");
    lvgl_ui_state_test_loop();
}
