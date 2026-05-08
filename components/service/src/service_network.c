/**
 * @file service_network.c
 * @brief 网络能力服务实现。
 */
#include "service_network.h"

#include "bsp_ml307c.h"
#include "bsp_uart.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_common.h"

#include <stddef.h>

static const char *TAG = "service_network";

#define SERVICE_NETWORK_DEFAULT_TIMEOUT_MS    5000u
#define SERVICE_NETWORK_DEFAULT_SOCKET_ID     1u

static ml307c_handle_t s_ml307c = NULL;
static uint8_t s_tcp_connected = 0u;
static uint8_t s_udp_connected = 0u;
static osal_mutex_t s_network_mutex = NULL;

static int service_network_is_inited(void)
{
    return s_ml307c != NULL;
}

static int service_network_reg_is_ready(int reg_state)
{
    return reg_state == 1 || reg_state == 5;
}

static int service_network_lock(uint32_t timeout_ms)
{
    if (s_network_mutex == NULL) {
        return -1;
    }

    return osal_mutex_lock(s_network_mutex, timeout_ms);
}

static void service_network_unlock(void)
{
    if (s_network_mutex != NULL) {
        osal_mutex_unlock(s_network_mutex);
    }
}

int service_network_init(const service_network_config_t *cfg)
{
    if (service_network_is_inited()) {
        SERVICE_LOGI(TAG, "网络服务已初始化");
        return 0;
    }

    ml307c_config_t ml307c_cfg = {
        .timeout_ms = SERVICE_NETWORK_DEFAULT_TIMEOUT_MS,
        .socket_id = SERVICE_NETWORK_DEFAULT_SOCKET_ID,
    };

    if (cfg != NULL) {
        if (cfg->at_timeout_ms != 0u) {
            ml307c_cfg.timeout_ms = cfg->at_timeout_ms;
        }
        if (cfg->socket_id != 0u) {
            ml307c_cfg.socket_id = cfg->socket_id;
        }
    }

    ml307c_interface_t itf = {
        .uart_write = ml307c_uart_write_impl,
        .uart_read = ml307c_uart_read_impl,
        .delay_ms = osal_delay_ms,
        .get_tick = osal_get_tick_ms,
    };

    if (s_network_mutex == NULL) {
        s_network_mutex = osal_mutex_create();
        if (s_network_mutex == NULL) {
            SERVICE_LOGE(TAG, "网络服务初始化失败: 互斥锁创建失败");
            return -3;
        }
    }

    s_ml307c = ml307c_init(&ml307c_cfg, &itf);
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: ML307C 对象创建失败");
        return -1;
    }

    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        return ret;
    }

    ret = ml307c_check_alive(s_ml307c);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: AT 通信失败, ret=%d", ret);
        service_network_unlock();
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        return ret;
    }

    ret = ml307c_check_sim(s_ml307c);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: SIM 卡状态异常, ret=%d", ret);
        service_network_unlock();
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        return ret;
    }
    service_network_unlock();

    s_tcp_connected = 0u;
    s_udp_connected = 0u;
    SERVICE_LOGI(TAG,
                 "网络服务初始化成功, timeout=%u, socket_id=%u",
                 (unsigned int)ml307c_cfg.timeout_ms,
                 (unsigned int)ml307c_cfg.socket_id);
    return 0;
}

int service_network_deinit(void)
{
    if (s_ml307c != NULL) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
    }

    s_tcp_connected = 0u;
    s_udp_connected = 0u;
    SERVICE_LOGI(TAG, "网络服务已释放");
    return 0;
}

int service_network_get_status(service_network_status_t *status)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "获取网络状态失败: 服务未初始化");
        return -1;
    }
    if (status == NULL) {
        SERVICE_LOGE(TAG, "获取网络状态失败: status 为空");
        return -2;
    }

    status->rssi = -1;
    status->reg_state = -1;
    status->link_state = -1;
    status->sim_ready = 0;
    status->at_ready = 0;

    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "获取网络状态失败: UART 互斥锁超时");
        return ret;
    }

    ret = ml307c_check_alive(s_ml307c);
    if (ret == 0) {
        status->at_ready = 1;
    } else {
        SERVICE_LOGW(TAG, "AT 状态查询失败, ret=%d", ret);
    }

    ret = ml307c_check_sim(s_ml307c);
    if (ret == 0) {
        status->sim_ready = 1;
    } else {
        SERVICE_LOGW(TAG, "SIM 状态查询失败, ret=%d", ret);
    }

    ret = ml307c_get_signal(s_ml307c, &status->rssi);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "信号强度查询失败, ret=%d", ret);
    }

    ret = ml307c_get_network_state(s_ml307c, &status->reg_state);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "网络注册状态查询失败, ret=%d", ret);
    }

    ret = ml307c_get_link_state(s_ml307c, &status->link_state);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "数据链路状态查询失败, ret=%d", ret);
    }

    SERVICE_LOGI(TAG,
                 "网络状态: at=%d, sim=%d, rssi=%d, cereg=%d, islink=%d",
                 status->at_ready,
                 status->sim_ready,
                 status->rssi,
                 status->reg_state,
                 status->link_state);
    service_network_unlock();
    return 0;
}

int service_network_is_ready(void)
{
    service_network_status_t status;
    int ret = service_network_get_status(&status);
    if (ret != 0) {
        return ret;
    }

    if (status.at_ready != 1 || status.sim_ready != 1) {
        return 0;
    }
    if (!service_network_reg_is_ready(status.reg_state)) {
        return 0;
    }
    if (status.link_state != 1) {
        return 0;
    }

    return 1;
}

int service_network_tcp_connect(const char *host, int port)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "TCP 连接失败: 服务未初始化");
        return -1;
    }
    if (host == NULL || port <= 0) {
        SERVICE_LOGE(TAG, "TCP 连接参数无效, host=%p, port=%d", host, port);
        return -2;
    }

    SERVICE_LOGI(TAG, "开始 TCP 连接, host=%s, port=%d", host, port);
    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_tcp_connect(s_ml307c, host, port);
    service_network_unlock();
    if (ret != 0) {
        s_tcp_connected = 0u;
        SERVICE_LOGE(TAG, "TCP 连接失败, ret=%d", ret);
        return ret;
    }

    s_tcp_connected = 1u;
    SERVICE_LOGI(TAG, "TCP 连接成功");
    return 0;
}

int service_network_tcp_send(const uint8_t *data, int len)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "TCP 发送失败: 服务未初始化");
        return -1;
    }
    if (data == NULL || len <= 0) {
        SERVICE_LOGE(TAG, "TCP 发送参数无效, data=%p, len=%d", data, len);
        return -2;
    }
    if (s_tcp_connected == 0u) {
        SERVICE_LOGW(TAG, "TCP 发送时通道未标记为已连接，仍尝试发送");
    }

    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_tcp_send(s_ml307c, (uint8_t *)data, len);
    service_network_unlock();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "TCP 发送失败, ret=%d", ret);
        return ret;
    }

    SERVICE_LOGI(TAG, "TCP 发送成功, len=%d", len);
    return 0;
}

int service_network_tcp_close(void)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "TCP 关闭失败: 服务未初始化");
        return -1;
    }

    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_tcp_close(s_ml307c);
    service_network_unlock();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "TCP 关闭失败, ret=%d", ret);
        return ret;
    }

    s_tcp_connected = 0u;
    SERVICE_LOGI(TAG, "TCP 已关闭");
    return 0;
}

int service_network_udp_connect(const char *host, int port)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "UDP 连接失败: 服务未初始化");
        return -1;
    }
    if (host == NULL || port <= 0) {
        SERVICE_LOGE(TAG, "UDP 连接参数无效, host=%p, port=%d", host, port);
        return -2;
    }

    SERVICE_LOGI(TAG, "开始 UDP DTU 配置, host=%s, port=%d", host, port);
    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_udp_connect(s_ml307c, host, port);
    service_network_unlock();
    if (ret != 0) {
        s_udp_connected = 0u;
        SERVICE_LOGE(TAG, "UDP DTU 配置失败, ret=%d", ret);
        return ret;
    }

    s_udp_connected = 1u;
    SERVICE_LOGI(TAG, "UDP DTU 配置成功");
    return 0;
}

int service_network_udp_send(const uint8_t *data, int len)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "UDP 发送失败: 服务未初始化");
        return -1;
    }
    if (data == NULL || len <= 0) {
        SERVICE_LOGE(TAG, "UDP 发送参数无效, data=%p, len=%d", data, len);
        return -2;
    }
    if (s_udp_connected == 0u) {
        SERVICE_LOGW(TAG, "UDP 发送时通道未标记为已连接，仍尝试发送");
    }

    int ret = service_network_lock(SERVICE_NETWORK_DEFAULT_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_udp_send(s_ml307c, (uint8_t *)data, len);
    service_network_unlock();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "UDP 发送失败, ret=%d", ret);
        return ret;
    }

    return 0;
}

int service_network_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (s_ml307c == NULL) {
        return -1;
    }
    if (buf == NULL || len == 0u) {
        return -2;
    }

    if (service_network_lock(timeout_ms + 20u) != 0) {
        return 0;
    }

    int ret = ml307c_read_raw(s_ml307c, buf, len, timeout_ms);
    service_network_unlock();
    return ret < 0 ? ret : ret;
}

int service_network_http_post_wav(const char *url,
                                  const uint8_t *wav,
                                  uint16_t wav_len,
                                  uint8_t *resp,
                                  uint16_t resp_size,
                                  uint16_t *resp_len)
{
    if (s_ml307c == NULL) {
        SERVICE_LOGE(TAG, "HTTP POST 失败: 服务未初始化");
        return -1;
    }
    if (url == NULL || wav == NULL || wav_len == 0u || resp == NULL || resp_len == NULL) {
        SERVICE_LOGE(TAG, "HTTP POST 参数无效");
        return -2;
    }

    int ret = service_network_lock(60000u);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "HTTP POST 失败: UART 互斥锁超时");
        return ret;
    }

    ret = ml307c_http_post(s_ml307c,
                           1u,
                           url,
                           "Content-Type: audio/wav",
                           wav,
                           wav_len,
                           resp,
                           resp_size,
                           resp_len);
    service_network_unlock();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "HTTP POST 失败, ret=%d", ret);
        return ret;
    }

    SERVICE_LOGI(TAG, "HTTP POST 完成, request=%u, response=%u",
                 (unsigned int)wav_len,
                 (unsigned int)*resp_len);
    return 0;
}
