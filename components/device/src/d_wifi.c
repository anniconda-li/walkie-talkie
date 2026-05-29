/**
 * @file d_wifi.c
 * @brief WiFi STA 网络驱动实现。
 */
#include "d_wifi.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "d_config.h"
#include "osal_task.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEVICE_WIFI_CONNECT_TIMEOUT_MS 30000u /**< WiFi 等待获取 IP 的最长时间。 */
#define DEVICE_WIFI_POLL_MS            200u   /**< WiFi 连接状态轮询间隔。 */

/** @brief WiFi 驱动日志标签。 */
static const char *TAG = "network_wifi";

/** @brief WiFi STA 是否已经启动。 */
static volatile int s_wifi_started = 0;

/** @brief WiFi STA 是否已经获取 IP。 */
static volatile int s_wifi_got_ip = 0;

/** @brief UDP socket 句柄。 */
static int s_udp_sock = -1;

/** @brief TCP socket 句柄。 */
static int s_tcp_sock = -1;

/** @brief UDP 对端地址缓存。 */
static struct sockaddr_storage s_udp_peer;

/** @brief UDP 对端地址长度。 */
static socklen_t s_udp_peer_len = 0;

/**
 * @brief 将 ESP-IDF/socket 错误码统一转换为本层负值错误码。
 */
static int device_wifi_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/**
 * @brief 处理 WiFi 和 IP 事件，维护启动和联网状态。
 */
static void device_wifi_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_started = 1;
        D_LOGI(TAG, "WiFi STA start");
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_got_ip = 0;
        D_LOGW(TAG, "WiFi STA disconnected, reason=%d", event != NULL ? event->reason : -1);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_got_ip = 1;
        if (event != NULL) {
            D_LOGI(TAG, "WiFi STA got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        } else {
            D_LOGI(TAG, "WiFi STA got ip");
        }
    }
}

/**
 * @brief 等待 WiFi STA 获取 IP 地址。
 */
static int device_wifi_wait_ip(void)
{
    uint32_t start = osal_get_tick_ms();
    while ((osal_get_tick_ms() - start) < DEVICE_WIFI_CONNECT_TIMEOUT_MS) {
        if (s_wifi_got_ip) {
            return 0;
        }
        osal_delay_ms(DEVICE_WIFI_POLL_MS);
    }

    return -1;
}

/**
 * @brief 初始化 WiFi STA 并等待联网完成。
 */
int d_wifi_init(const d_wifi_config_t *cfg)
{
    if (cfg == NULL || cfg->ssid == NULL || cfg->ssid[0] == '\0') {
        return -1;
    }

    if (s_wifi_started) {
        return device_wifi_wait_ip();
    }

    int ret = nvs_flash_init();
    if (ret != 0) {
        (void)nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    ret = esp_netif_init();
    if (ret != 0 && ret != ESP_ERR_INVALID_STATE) {
        return device_wifi_err_to_int(ret);
    }

    ret = esp_event_loop_create_default();
    if (ret != 0 && ret != ESP_ERR_INVALID_STATE) {
        return device_wifi_err_to_int(ret);
    }

    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != 0 && ret != ESP_ERR_WIFI_INIT_STATE) {
        return device_wifi_err_to_int(ret);
    }

    (void)esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              device_wifi_event_handler,
                                              NULL,
                                              NULL);
    (void)esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              device_wifi_event_handler,
                                              NULL,
                                              NULL);

    wifi_config_t wifi_cfg = {0};
    (void)strncpy((char *)wifi_cfg.sta.ssid,
                  cfg->ssid,
                  sizeof(wifi_cfg.sta.ssid) - 1u);
    (void)strncpy((char *)wifi_cfg.sta.password,
                  cfg->password != NULL ? cfg->password : "",
                  sizeof(wifi_cfg.sta.password) - 1u);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == 0) {
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    }
    if (ret == 0) {
        ret = esp_wifi_start();
    }
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    D_LOGI(TAG, "WiFi STA 启动完成, ssid=%s", cfg->ssid);
    return device_wifi_wait_ip();
}

/**
 * @brief 关闭 WiFi 网络层持有的 socket。
 */
int d_wifi_deinit(void)
{
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }
    return 0;
}

/**
 * @brief 判断 WiFi STA 是否已经启动。
 */
int d_wifi_is_initialized(void)
{
    return s_wifi_started ? 1 : 0;
}

/**
 * @brief 将 WiFi RSSI dBm 映射为类似 CSQ 的 0-31 信号值。
 */
static int device_wifi_rssi_to_csq(int rssi_dbm)
{
    if (rssi_dbm <= -100) {
        return 0;
    }
    if (rssi_dbm >= -50) {
        return 31;
    }
    return (rssi_dbm + 100) * 31 / 50;
}

/**
 * @brief 获取 WiFi 当前链路状态。
 */
int d_wifi_get_status(d_wifi_status_t *status)
{
    if (status == NULL) {
        return -1;
    }

    status->link_ready = s_wifi_got_ip ? 1 : 0;
    status->rssi = 99;

    wifi_ap_record_t ap = {0};
    if (s_wifi_got_ip && esp_wifi_sta_get_ap_info(&ap) == 0) {
        status->rssi = device_wifi_rssi_to_csq(ap.rssi);
    }

    return 0;
}

/**
 * @brief 判断 WiFi 是否已经获取 IP，可用于上层发送前检查。
 */
int d_wifi_is_ready(void)
{
    return s_wifi_got_ip ? 1 : 0;
}

/**
 * @brief 解析主机名和端口为 IPv4 socket 地址。
 */
static int device_wifi_resolve(const char *host,
                               int port,
                               int socktype,
                               struct sockaddr_storage *addr,
                               socklen_t *addr_len)
{
    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = socktype,
    };
    struct addrinfo *res = NULL;

    snprintf(port_str, sizeof(port_str), "%d", port);
    int ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0 || res == NULL) {
        return -1;
    }

    memcpy(addr, res->ai_addr, res->ai_addrlen);
    *addr_len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

/**
 * @brief 建立 UDP 发送目标并创建 UDP socket。
 */
int d_wifi_udp_connect(const char *host, int port)
{
    if (!s_wifi_got_ip || host == NULL || port <= 0) {
        return -1;
    }

    if (device_wifi_resolve(host, port, SOCK_DGRAM, &s_udp_peer, &s_udp_peer_len) != 0) {
        return -2;
    }

    if (s_udp_sock >= 0) {
        close(s_udp_sock);
    }
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    return s_udp_sock >= 0 ? 0 : -3;
}

/**
 * @brief 通过已配置的 UDP 对端发送数据。
 */
int d_wifi_udp_send(const uint8_t *data, int len)
{
    if (s_udp_sock < 0 || data == NULL || len <= 0 || s_udp_peer_len == 0) {
        return -1;
    }

    int ret = sendto(s_udp_sock, data, (size_t)len, 0, (struct sockaddr *)&s_udp_peer, s_udp_peer_len);
    if (ret == len) {
        return 0;
    }

    D_LOGW(TAG, "UDP 发送失败, len=%d, ret=%d, errno=%d", len, ret, errno);
    return -2;
}

/**
 * @brief 从 UDP socket 读取下行数据。
 */
int d_wifi_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (s_udp_sock < 0 || buf == NULL || len == 0u) {
        return -1;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(s_udp_sock, &read_fds);

    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };

    int ret = select(s_udp_sock + 1, &read_fds, NULL, NULL, &tv);
    if (ret <= 0) {
        return 0;
    }

    return recvfrom(s_udp_sock, buf, len, 0, NULL, NULL);
}

/**
 * @brief 建立 TCP 连接。
 */
int d_wifi_tcp_connect(const char *host, int port)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    if (!s_wifi_got_ip || host == NULL || port <= 0 ||
        device_wifi_resolve(host, port, SOCK_STREAM, &addr, &addr_len) != 0) {
        return -1;
    }

    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
    }
    s_tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_tcp_sock < 0) {
        return -2;
    }

    int ret = connect(s_tcp_sock, (struct sockaddr *)&addr, addr_len);
    return ret == 0 ? 0 : -3;
}

/**
 * @brief 通过已连接的 TCP socket 发送数据。
 */
int d_wifi_tcp_send(const uint8_t *data, int len)
{
    if (s_tcp_sock < 0 || data == NULL || len <= 0) {
        return -1;
    }

    int ret = send(s_tcp_sock, data, (size_t)len, 0);
    return ret == len ? 0 : -2;
}

/**
 * @brief 关闭当前 TCP socket。
 */
int d_wifi_tcp_close(void)
{
    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }
    return 0;
}

/**
 * @brief 通过 ESP HTTP client 执行一次 HTTP POST。
 */
int d_wifi_http_post(const char *url,
                          const char *content_type,
                          const uint8_t *body,
                          uint32_t body_len,
                          uint8_t *resp,
                          uint32_t resp_size,
                          uint32_t *resp_len,
                          uint32_t timeout_ms)
{
    if (!s_wifi_got_ip || url == NULL || resp == NULL || resp_len == NULL ||
        (body_len > 0u && body == NULL)) {
        return -1;
    }
    if (body_len > (uint32_t)INT32_MAX || resp_size > (uint32_t)INT32_MAX) {
        return -2;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = (int)(timeout_ms == 0u ? 30000u : timeout_ms),
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -3;
    }

    int ret = 0;
    *resp_len = 0u;
    (void)esp_http_client_set_method(client, HTTP_METHOD_POST);
    (void)esp_http_client_set_header(client,
                                     "Content-Type",
                                     content_type != NULL ? content_type : "application/octet-stream");

    ret = device_wifi_err_to_int(esp_http_client_open(client, (int)body_len));
    if (ret == 0) {
        int written = 0;
        if (body_len > 0u) {
            written = esp_http_client_write(client, (const char *)body, (int)body_len);
        }
        if (written != (int)body_len) {
            ret = -4;
        }
    }
    if (ret == 0) {
        int header_len = esp_http_client_fetch_headers(client);
        if (header_len < 0) {
            ret = header_len;
        }
    }
    if (ret == 0) {
        int read_len = esp_http_client_read_response(client, (char *)resp, (int)resp_size);
        if (read_len < 0) {
            ret = read_len;
        } else {
            *resp_len = (uint32_t)read_len;
        }
    }

    esp_http_client_cleanup(client);
    return ret;
}
