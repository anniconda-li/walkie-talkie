/**
 * @file main.c
 * @brief 网络服务 TCP 发送测试入口。
 */

#include "osal_log.h"
#include "service_network.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "network_test";

#define NETWORK_TEST_TIMEOUT_MS      5000u
#define NETWORK_TEST_TCP_HOST        "139.129.17.67"
#define NETWORK_TEST_TCP_PORT        55555
#define NETWORK_TEST_TCP_PAYLOAD     "hello tcp test\r\n"

static int network_test_log_status(void)
{
    service_network_status_t status;
    int ret = service_network_get_status(&status);
    if (ret != 0) {
        OSAL_LOGE(TAG, "网络状态查询失败, ret=%d", ret);
        return ret;
    }

    OSAL_LOGI(TAG,
              "网络状态: at=%d, sim=%d, rssi=%d, cereg=%d, islink=%d",
              status.at_ready,
              status.sim_ready,
              status.rssi,
              status.reg_state,
              status.link_state);
    return 0;
}

static int network_test_tcp_send(void)
{
    OSAL_LOGI(TAG,
              "开始 TCP 测试, host=%s, port=%d",
              NETWORK_TEST_TCP_HOST,
              NETWORK_TEST_TCP_PORT);

    int ret = service_network_tcp_connect(NETWORK_TEST_TCP_HOST, NETWORK_TEST_TCP_PORT);
    if (ret != 0) {
        OSAL_LOGE(TAG, "TCP 连接失败, ret=%d", ret);
        return ret;
    }
    OSAL_LOGI(TAG, "TCP 通道连接成功");

    const uint8_t payload[] = NETWORK_TEST_TCP_PAYLOAD;
    ret = service_network_tcp_send(payload, (int)strlen((const char *)payload));
    if (ret != 0) {
        OSAL_LOGE(TAG, "TCP 数据发送失败, ret=%d", ret);
        return ret;
    }

    OSAL_LOGI(TAG, "TCP 数据发送完成: %s", NETWORK_TEST_TCP_PAYLOAD);
    return 0;
}

void app_main(void)
{
    OSAL_LOGI(TAG, "开始网络服务 TCP 发送测试");

    service_network_config_t cfg = {
        .at_timeout_ms = NETWORK_TEST_TIMEOUT_MS,
        .socket_id = 1u,
    };

    int ret = service_network_init(&cfg);
    if (ret != 0) {
        OSAL_LOGE(TAG, "网络服务初始化失败, ret=%d", ret);
        return;
    }

    (void)network_test_log_status();

    ret = service_network_is_ready();
    if (ret == 1) {
        (void)network_test_tcp_send();
    } else if (ret == 0) {
        OSAL_LOGE(TAG, "网络尚未就绪，跳过 TCP 发送");
    } else {
        OSAL_LOGE(TAG, "网络就绪状态查询失败, ret=%d", ret);
    }

    (void)service_network_deinit();
    OSAL_LOGI(TAG, "网络服务 TCP 发送测试结束");
}
