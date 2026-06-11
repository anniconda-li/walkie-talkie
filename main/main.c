/**
 * @file main.c
 * @brief 新板 4G 功能测试入口。
 */

#include "d_ml307c.h"
#include "osal_log.h"
#include "osal_task.h"
#include "wdriver_config.h"
#include "wdriver_uart.h"

static const char *TAG = "main_4g_test";

#define MAIN_4G_TEST_TIMEOUT_MS 10000u
#define MAIN_4G_TEST_SOCKET_ID  1u
#define MAIN_4G_TEST_PERIOD_MS  5000u

static const char *main_4g_reg_state_name(int state)
{
    switch (state) {
        case 0:
            return "not_registered";
        case 1:
            return "registered_home";
        case 2:
            return "searching";
        case 3:
            return "registration_denied";
        case 4:
            return "unknown";
        case 5:
            return "registered_roaming";
        default:
            return "invalid";
    }
}

static int main_4g_signal_bars(int rssi)
{
    if (rssi == 99 || rssi < 0) {
        return 0;
    }
    if (rssi >= 20) {
        return 4;
    }
    if (rssi >= 15) {
        return 3;
    }
    if (rssi >= 10) {
        return 2;
    }
    if (rssi >= 2) {
        return 1;
    }
    return 0;
}

static void main_4g_log_status(const d_ml307c_status_t *status)
{
    OSAL_LOGI(TAG,
              "4G status: at=%d sim=%d cereg=%d(%s) link=%d csq=%d bars=%d",
              status->at_ready,
              status->sim_ready,
              status->reg_state,
              main_4g_reg_state_name(status->reg_state),
              status->link_state,
              status->rssi,
              main_4g_signal_bars(status->rssi));
}

void app_main(void)
{
    OSAL_LOGI(TAG, "new board 4G test start");
    OSAL_LOGI(TAG, "UART: port=%d baud=%d tx=%d rx=%d",
              WDRIVER_UART_PORT,
              WDRIVER_UART_BAUD_RATE,
              WDRIVER_UART_TX_IO,
              WDRIVER_UART_RX_IO);

    int ret = wdriver_uart_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "UART init failed, ret=%d", ret);
        while (1) {
            osal_delay_ms(1000u);
        }
    }

    d_ml307c_wdriver_ops_t ops = {
        .uart_write = wdriver_uart_write,
        .uart_read = wdriver_uart_read,
        .delay_ms = osal_delay_ms,
        .get_tick_ms = osal_get_tick_ms,
    };
    ml307c_config_t cfg = {
        .timeout_ms = MAIN_4G_TEST_TIMEOUT_MS,
        .socket_id = MAIN_4G_TEST_SOCKET_ID,
    };

    ret = d_ml307c_prepare(&ops, &cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "ML307C prepare failed, ret=%d", ret);
    } else {
        OSAL_LOGI(TAG, "ML307C prepare ok");
    }

    while (1) {
        d_ml307c_status_t status = {0};
        ret = d_ml307c_probe(&status);
        if (ret == 0) {
            main_4g_log_status(&status);
            if (status.at_ready == 1 &&
                status.sim_ready == 1 &&
                (status.reg_state == 1 || status.reg_state == 5) &&
                status.link_state == 1) {
                OSAL_LOGI(TAG, "4G test PASS: module, SIM, network registration and link are ready");
            } else {
                OSAL_LOGW(TAG, "4G test waiting: module/SIM/registration/link not fully ready");
            }
        } else {
            OSAL_LOGE(TAG, "ML307C probe failed, ret=%d", ret);
        }

        osal_delay_ms(MAIN_4G_TEST_PERIOD_MS);
    }
}
