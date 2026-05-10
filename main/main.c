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
#include "service_network.h"

#include <stdio.h>

#ifndef APP_MAIN_WIFI_TEST
#define APP_MAIN_WIFI_TEST 1
#endif

#ifndef APP_MAIN_WIFI_TEST_HOST
#define APP_MAIN_WIFI_TEST_HOST "10.251.162.251"
#endif

#ifndef APP_MAIN_WIFI_TEST_UDP_PORT
#define APP_MAIN_WIFI_TEST_UDP_PORT 33333
#endif

#ifndef APP_MAIN_WIFI_TEST_TCP_PORT
#define APP_MAIN_WIFI_TEST_TCP_PORT 33334
#endif

#if APP_MAIN_WIFI_TEST
#if DRIVER_INIT_NETWORK != DRIVER_INIT_NETWORK_WIFI
#error "APP_MAIN_WIFI_TEST requires DRIVER_INIT_NETWORK_WIFI"
#endif
#if SERVICE_INIT_NETWORK != SERVICE_INIT_NETWORK_WIFI
#error "APP_MAIN_WIFI_TEST requires SERVICE_INIT_NETWORK_WIFI"
#endif
#endif

/**
 * @brief 应用日志标签。
 */
static const char *TAG = "walkie_app";

#if APP_MAIN_WIFI_TEST
static void app_main_wifi_test_transfer(uint32_t seq)
{
    char tx[96];
    uint8_t rx[128];

    int tx_len = snprintf(tx, sizeof(tx), "walkie wifi test seq=%lu", (unsigned long)seq);
    if (tx_len <= 0 || tx_len >= (int)sizeof(tx)) {
        OSAL_LOGE(TAG, "WiFi 测试消息生成失败, seq=%lu", (unsigned long)seq);
        return;
    }

    int ret = service_network_udp_connect(APP_MAIN_WIFI_TEST_HOST, APP_MAIN_WIFI_TEST_UDP_PORT);
    if (ret == 0) {
        ret = service_network_udp_send((const uint8_t *)tx, tx_len);
    }
    if (ret == 0) {
        int rx_len = service_network_read_downlink(rx, sizeof(rx), 1000u);
        if (rx_len > 0) {
            OSAL_LOGI(TAG,
                      "UDP 测试成功: sent=%d recv=%d data=%.*s",
                      tx_len,
                      rx_len,
                      rx_len,
                      (const char *)rx);
        } else {
            OSAL_LOGW(TAG, "UDP 测试未收到回包: sent=%d recv_ret=%d", tx_len, rx_len);
        }
    } else {
        OSAL_LOGW(TAG, "UDP 测试发送失败, ret=%d", ret);
    }

    ret = service_network_tcp_connect(APP_MAIN_WIFI_TEST_HOST, APP_MAIN_WIFI_TEST_TCP_PORT);
    if (ret == 0) {
        ret = service_network_tcp_send((const uint8_t *)tx, tx_len);
    }
    (void)service_network_tcp_close();

    if (ret == 0) {
        OSAL_LOGI(TAG, "TCP 测试发送成功: len=%d", tx_len);
    } else {
        OSAL_LOGW(TAG, "TCP 测试失败, ret=%d", ret);
    }
}

static void app_main_wifi_test_loop(void)
{
    uint32_t seq = 0u;

    while (1) {
        service_network_status_t status = {0};
        int ret = service_network_get_status(&status);
        int ready = service_network_is_ready();

        OSAL_LOGI(TAG,
                  "WiFi 测试: ret=%d ready=%d at=%d sim=%d reg=%d link=%d rssi=%d",
                  ret,
                  ready,
                  status.at_ready,
                  status.sim_ready,
                  status.reg_state,
                  status.link_state,
                  status.rssi);

        if (ready == 1) {
            app_main_wifi_test_transfer(seq++);
        }
        osal_delay_ms(3000u);
    }
}
#endif

void app_main(void)
{
    OSAL_LOGI(TAG, "开始启动业务应用");

    int ret = bsp_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "BSP 基础资源初始化失败, ret=%d", ret);
        return;
    }

#if APP_MAIN_WIFI_TEST
    ret = driver_network_init();
    if (ret != 0) {
        OSAL_LOGE(TAG, "WiFi Driver 初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    ret = service_init_network();
    if (ret != 0) {
        OSAL_LOGE(TAG, "WiFi Service 初始化失败, ret=%d", ret);
        (void)bsp_deinit();
        return;
    }

    OSAL_LOGI(TAG, "WiFi 测试启动完成");
    app_main_wifi_test_loop();
#else
    ret = driver_init();
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
#endif
}
