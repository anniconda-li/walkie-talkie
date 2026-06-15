/**
 * @file service_network.c
 * @brief 网络服务能力门面实现。
 *
 * service_network 只保存一份由 service_init 传入的网络能力函数表，
 * 对 app 层提供稳定的网络 API。这里不包含 WiFi socket 细节，具体差异
 * 全部由下层 driver 的 ops 实现承担。
 */
#include "service_network.h"

#include "service_config.h"
#include "osal_mutex.h"

#include <stddef.h>
#include <stdint.h>

/** @brief 网络 service 日志标签。 */
static const char *TAG = "service_network";

/** @brief 当前绑定的网络 driver 能力函数表。 */
static service_network_ops_t s_network_ops;

/** @brief 网络 ops 是否已经完成绑定并通过初始化检查。 */
static uint8_t s_network_ops_ready = 0u;

/** @brief 串行化网络 I/O，避免 HTTP 与 UDP/TCP 同时进入底层网络路径。 */
static osal_mutex_t s_network_io_mutex = NULL;

static int service_network_lock_io(void)
{
    if (s_network_io_mutex == NULL) {
        s_network_io_mutex = osal_mutex_create();
        if (s_network_io_mutex == NULL) {
            return -1;
        }
    }

    return osal_mutex_lock(s_network_io_mutex, OSAL_WAIT_FOREVER);
}

static void service_network_unlock_io(void)
{
    osal_mutex_unlock(s_network_io_mutex);
}

/**
 * @brief 检查网络服务所需的下层能力是否完整。
 *
 * 这里校验的是 service 运行所需的最小能力集合：状态查询、TCP、UDP、
 * 下行读取和 HTTP POST。只要任一函数为空，service 就不允许初始化，
 * 避免运行时空函数指针崩溃。
 *
 * @param[in] ops 待检查的网络能力函数表。
 * @return 有效返回 0；无效返回 -1。
 */
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
        ops->http_post == NULL) {
        return -1;
    }

    return 0;
}

int service_network_init(const service_network_ops_t *ops)
{
    /*
     * ops == NULL 表示“只检查当前 service 是否已经初始化”。
     * 状态监控任务掉线重试时会调用这个路径，不重新选择 driver。
     */
    if (ops == NULL) {
        return s_network_ops_ready != 0u ? 0 : -1;
    }

    if (s_network_io_mutex == NULL) {
        s_network_io_mutex = osal_mutex_create();
        if (s_network_io_mutex == NULL) {
            SERVICE_LOGE(TAG, "网络服务初始化失败: I/O 互斥锁创建失败");
            s_network_ops_ready = 0u;
            return -4;
        }
    }

    if (service_network_ops_is_valid(ops) != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: ops 无效");
        s_network_ops_ready = 0u;
        return -2;
    }

    if (ops->is_initialized() != 1) {
        SERVICE_LOGE(TAG, "网络服务初始化失败: 下层网络 driver 未初始化");
        s_network_ops_ready = 0u;
        return -3;
    }

    if (service_network_lock_io() != 0) {
        return -5;
    }
    s_network_ops = *ops;
    s_network_ops_ready = 1u;
    service_network_unlock_io();
    return 0;
}

int service_network_deinit(void)
{
    if (s_network_io_mutex != NULL && service_network_lock_io() != 0) {
        return -1;
    }

    /* 清空函数表后，所有公开 API 都会被 s_network_ops_ready 拦住。 */
    s_network_ops = (service_network_ops_t){0};
    s_network_ops_ready = 0u;
    if (s_network_io_mutex != NULL) {
        service_network_unlock_io();
    }
    return 0;
}

int service_network_get_status(service_network_status_t *status)
{
    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.get_status == NULL) {
        service_network_unlock_io();
        return -1;
    }

    int ret = s_network_ops.get_status(status);
    service_network_unlock_io();
    return ret;
}

int service_network_is_ready(void)
{
    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.is_ready == NULL) {
        service_network_unlock_io();
        return -1;
    }

    int ret = s_network_ops.is_ready();
    service_network_unlock_io();
    return ret;
}

int service_network_tcp_connect(const char *host, int port)
{
    /* TCP 接口保留给简单连通性测试和后续业务扩展。 */
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.tcp_connect == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.tcp_connect(host, port);
    service_network_unlock_io();
    return ret;
}

int service_network_tcp_send(const uint8_t *data, int len)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.tcp_send == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.tcp_send(data, len);
    service_network_unlock_io();
    return ret;
}

int service_network_tcp_close(void)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.tcp_close == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.tcp_close();
    service_network_unlock_io();
    return ret;
}

int service_network_udp_connect(const char *host, int port)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.udp_connect == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.udp_connect(host, port);
    service_network_unlock_io();
    return ret;
}

int service_network_udp_send(const uint8_t *data, int len)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.udp_send == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.udp_send(data, len);
    service_network_unlock_io();
    return ret;
}

int service_network_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    /* 下行读取为轮询式接口，app_intercom 的 UDP 接收任务负责包重组。 */
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.read_downlink == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.read_downlink(buf, len, timeout_ms);
    service_network_unlock_io();
    return ret;
}

int service_network_http_post(const char *url,
                              const char *content_type,
                              const uint8_t *body,
                              uint32_t body_len,
                              uint8_t *resp,
                              uint32_t resp_size,
                              uint32_t *resp_len,
                              uint32_t timeout_ms)
{
    if (s_network_ops_ready == 0u) {
        return -1;
    }

    if (service_network_lock_io() != 0) {
        return -2;
    }
    if (s_network_ops_ready == 0u || s_network_ops.http_post == NULL) {
        service_network_unlock_io();
        return -1;
    }
    int ret = s_network_ops.http_post(url,
                                      content_type,
                                      body,
                                      body_len,
                                      resp,
                                      resp_size,
                                      resp_len,
                                      timeout_ms);
    service_network_unlock_io();
    return ret;
}
