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

/** @brief WiFi 内部资源是否已经初始化。 */
static volatile int s_wifi_prepared = 0;

/** @brief WiFi STA 是否已经获取 IP。 */
static volatile int s_wifi_got_ip = 0;

/** @brief 最近一次 STA 断开原因。 */
static volatile int s_wifi_disconnect_reason = 0;

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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_got_ip = 0;
        s_wifi_disconnect_reason = event != NULL ? event->reason : -1;
        D_LOGW(TAG, "WiFi STA disconnected, reason=%d", s_wifi_disconnect_reason);
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
static int device_wifi_reason_to_connect_error(int reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return -4;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return -5;
        case WIFI_REASON_ASSOC_FAIL:
            return -6;
        default:
            return 0;
    }
}

static int device_wifi_wait_ip(uint32_t timeout_ms)
{
    uint32_t start = osal_get_tick_ms();
    while ((osal_get_tick_ms() - start) < timeout_ms) {
        if (s_wifi_got_ip) {
            return 0;
        }
        int err = device_wifi_reason_to_connect_error(s_wifi_disconnect_reason);
        if (err != 0) {
            return err;
        }
        osal_delay_ms(DEVICE_WIFI_POLL_MS);
    }

    return -1;
}

int d_wifi_prepare(void)
{
    if (s_wifi_prepared) {
        return 0;
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

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == 0) {
        ret = esp_wifi_start();
    }
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    s_wifi_prepared = 1;
    D_LOGI(TAG, "WiFi STA 资源准备完成");
    return 0;
}

int d_wifi_scan(d_wifi_ap_record_t *records, uint16_t max_records, uint16_t *count)
{
    if (records == NULL || count == NULL || max_records == 0u) {
        return -1;
    }

    int ret = d_wifi_prepare();
    if (ret != 0) {
        return ret;
    }

    wifi_scan_config_t scan_cfg = {0};
    ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    uint16_t ap_count = max_records;
    wifi_ap_record_t ap_records[max_records];
    memset(ap_records, 0, sizeof(ap_records));
    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        strncpy(records[i].ssid, (const char *)ap_records[i].ssid, sizeof(records[i].ssid) - 1u);
        records[i].ssid[sizeof(records[i].ssid) - 1u] = '\0';
        records[i].rssi = ap_records[i].rssi;
        records[i].authmode = ap_records[i].authmode;
    }
    *count = ap_count;
    return 0;
}

int d_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }

    int ret = d_wifi_prepare();
    if (ret != 0) {
        return ret;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1u);
    strncpy((char *)wifi_cfg.sta.password,
            password != NULL ? password : "",
            sizeof(wifi_cfg.sta.password) - 1u);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    (void)esp_wifi_disconnect();
    osal_delay_ms(150u);
    s_wifi_got_ip = 0;
    s_wifi_disconnect_reason = 0;
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret == 0) {
        ret = esp_wifi_connect();
    }
    if (ret != 0) {
        return device_wifi_err_to_int(ret);
    }

    D_LOGI(TAG, "WiFi STA 开始连接, ssid=%s", ssid);
    return device_wifi_wait_ip(timeout_ms == 0u ? DEVICE_WIFI_CONNECT_TIMEOUT_MS : timeout_ms);
}

/**
 * @brief 初始化 WiFi STA 并等待联网完成。
 */
int d_wifi_init(const d_wifi_config_t *cfg)
{
    if (cfg == NULL || cfg->ssid == NULL || cfg->ssid[0] == '\0') {
        return -1;
    }

    return d_wifi_connect(cfg->ssid, cfg->password, DEVICE_WIFI_CONNECT_TIMEOUT_MS);
}

/**
 * @brief 断开 WiFi 并关闭网络层持有的 socket。
 */
int d_wifi_deinit(void)
{
    return d_wifi_disconnect();
}

int d_wifi_disconnect(void)
{
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }
    s_udp_peer_len = 0;
    s_wifi_got_ip = 0;
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
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
    status->ssid[0] = '\0';

    wifi_ap_record_t ap = {0};
    if (s_wifi_got_ip && esp_wifi_sta_get_ap_info(&ap) == 0) {
        status->rssi = device_wifi_rssi_to_csq(ap.rssi);
        strncpy(status->ssid, (const char *)ap.ssid, sizeof(status->ssid) - 1u);
        status->ssid[sizeof(status->ssid) - 1u] = '\0';
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
        uint32_t written_total = 0u;
        uint32_t write_start_ms = osal_get_tick_ms();
        while (written_total < body_len) {
            uint32_t remain = body_len - written_total;
            int write_len = (int)(remain > 4096u ? 4096u : remain);
            int written = esp_http_client_write(client,
                                                (const char *)&body[written_total],
                                                write_len);
            if (written <= 0) {
                ret = written < 0 ? written : -4;
                D_LOGW(TAG,
                       "HTTP POST body 写入失败, ret=%d, written=%u/%u",
                       ret,
                       (unsigned int)written_total,
                       (unsigned int)body_len);
                break;
            }
            written_total += (uint32_t)written;
        }
        if (ret == 0 && written_total != body_len) {
            ret = -4;
            D_LOGW(TAG,
                   "HTTP POST body 写入不完整, written=%u/%u",
                   (unsigned int)written_total,
                   (unsigned int)body_len);
        }
        if (ret == 0 && body_len >= 16384u) {
            D_LOGI(TAG,
                   "HTTP POST body 写入完成, body_len=%u, write_ms=%u",
                   (unsigned int)body_len,
                   (unsigned int)(osal_get_tick_ms() - write_start_ms));
        }
    }
    if (ret == 0) {
        int header_len = esp_http_client_fetch_headers(client);
        if (header_len < 0) {
            ret = header_len;
            D_LOGW(TAG,
                   "HTTP POST 等待响应头失败, ret=%d, body_len=%u",
                   ret,
                   (unsigned int)body_len);
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
