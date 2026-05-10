/**
 * @file service_network.c
 * @brief Network service capability facade.
 */
#include "service_network.h"

#include "service_config.h"

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "service_network";

static service_network_ops_t s_network_ops;
static uint8_t s_network_ops_ready = 0u;

static int service_network_ops_is_valid(const service_network_ops_t *ops)
{
    if (ops == NULL ||
        ops->is_initialized == NULL ||
        ops->get_status == NULL ||
        ops->is_ready == NULL ||
        ops->tcp_connect == NULL ||
        ops->tcp_send == NULL ||
        ops->tcp_close == NULL ||
        ops->udp_connect == NULL ||
        ops->udp_send == NULL ||
        ops->read_downlink == NULL ||
        ops->http_post_wav == NULL) {
        return -1;
    }

    return 0;
}

int service_network_init(const service_network_config_t *cfg)
{
    if (cfg == NULL) {
        return s_network_ops_ready != 0u ? 0 : -1;
    }

    if (service_network_ops_is_valid(&cfg->ops) != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: ops 无效");
        s_network_ops_ready = 0u;
        return -2;
    }

    if (cfg->ops.is_initialized() != 1) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: 下层网络 driver 未初始化");
        s_network_ops_ready = 0u;
        return -3;
    }

    s_network_ops = cfg->ops;
    s_network_ops_ready = 1u;
    return 0;
}

int service_network_deinit(void)
{
    s_network_ops = (service_network_ops_t){0};
    s_network_ops_ready = 0u;
    return 0;
}

int service_network_get_status(service_network_status_t *status)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.get_status(status);
}

int service_network_is_ready(void)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.is_ready();
}

int service_network_tcp_connect(const char *host, int port)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.tcp_connect(host, port);
}

int service_network_tcp_send(const uint8_t *data, int len)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.tcp_send(data, len);
}

int service_network_tcp_close(void)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.tcp_close();
}

int service_network_udp_connect(const char *host, int port)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.udp_connect(host, port);
}

int service_network_udp_send(const uint8_t *data, int len)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.udp_send(data, len);
}

int service_network_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.read_downlink(buf, len, timeout_ms);
}

int service_network_http_post_wav(const char *url,
                                  const uint8_t *wav,
                                  uint16_t wav_len,
                                  uint8_t *resp,
                                  uint16_t resp_size,
                                  uint16_t *resp_len)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    return s_network_ops.http_post_wav(url, wav, wav_len, resp, resp_size, resp_len);
}
