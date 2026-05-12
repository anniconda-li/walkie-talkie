/**
 * @file driver_wifi.c
 * @brief WiFi STA network driver implementation.
 */
#include "driver_wifi.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver_config.h"
#include "osal_task.h"

#include <errno.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEVICE_WIFI_CONNECT_TIMEOUT_MS 30000u
#define DEVICE_WIFI_POLL_MS            200u

static const char *TAG = "network_wifi";

static volatile int s_wifi_started = 0;
static volatile int s_wifi_got_ip = 0;
static int s_udp_sock = -1;
static int s_tcp_sock = -1;
static struct sockaddr_storage s_udp_peer;
static socklen_t s_udp_peer_len = 0;

static int device_wifi_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

static void device_wifi_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_started = 1;
        DRIVER_LOGI(TAG, "WiFi STA start");
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_got_ip = 0;
        DRIVER_LOGW(TAG, "WiFi STA disconnected, reason=%d", event != NULL ? event->reason : -1);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_got_ip = 1;
        if (event != NULL) {
            DRIVER_LOGI(TAG, "WiFi STA got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        } else {
            DRIVER_LOGI(TAG, "WiFi STA got ip");
        }
    }
}

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

int driver_wifi_init(const driver_wifi_config_t *cfg)
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

    DRIVER_LOGI(TAG, "WiFi STA 启动完成, ssid=%s", cfg->ssid);
    return device_wifi_wait_ip();
}

int driver_wifi_deinit(void)
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

int driver_wifi_is_initialized(void)
{
    return s_wifi_started ? 1 : 0;
}

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

int driver_wifi_get_status(driver_wifi_status_t *status)
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

int driver_wifi_is_ready(void)
{
    return s_wifi_got_ip ? 1 : 0;
}

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

int driver_wifi_udp_connect(const char *host, int port)
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

int driver_wifi_udp_send(const uint8_t *data, int len)
{
    if (s_udp_sock < 0 || data == NULL || len <= 0 || s_udp_peer_len == 0) {
        return -1;
    }

    int ret = sendto(s_udp_sock, data, (size_t)len, 0, (struct sockaddr *)&s_udp_peer, s_udp_peer_len);
    if (ret == len) {
        return 0;
    }

    DRIVER_LOGW(TAG, "UDP 发送失败, len=%d, ret=%d, errno=%d", len, ret, errno);
    return -2;
}

int driver_wifi_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
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

int driver_wifi_tcp_connect(const char *host, int port)
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

int driver_wifi_tcp_send(const uint8_t *data, int len)
{
    if (s_tcp_sock < 0 || data == NULL || len <= 0) {
        return -1;
    }

    int ret = send(s_tcp_sock, data, (size_t)len, 0);
    return ret == len ? 0 : -2;
}

int driver_wifi_tcp_close(void)
{
    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }
    return 0;
}

int driver_wifi_http_post_wav(const char *url,
                              const uint8_t *wav,
                              uint16_t wav_len,
                              uint8_t *resp,
                              uint16_t resp_size,
                              uint16_t *resp_len,
                              uint32_t timeout_ms)
{
    if (!s_wifi_got_ip || url == NULL || wav == NULL || resp == NULL || resp_len == NULL) {
        return -1;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = (int)(timeout_ms == 0u ? 30000u : timeout_ms),
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -2;
    }

    int ret = 0;
    *resp_len = 0u;
    (void)esp_http_client_set_method(client, HTTP_METHOD_POST);
    (void)esp_http_client_set_header(client, "Content-Type", "audio/wav");

    ret = device_wifi_err_to_int(esp_http_client_open(client, wav_len));
    if (ret == 0) {
        int written = esp_http_client_write(client, (const char *)wav, wav_len);
        if (written != wav_len) {
            ret = -3;
        }
    }
    if (ret == 0) {
        int header_len = esp_http_client_fetch_headers(client);
        if (header_len < 0) {
            ret = header_len;
        }
    }
    if (ret == 0) {
        int read_len = esp_http_client_read_response(client, (char *)resp, resp_size);
        if (read_len < 0) {
            ret = read_len;
        } else {
            *resp_len = (uint16_t)read_len;
        }
    }

    esp_http_client_cleanup(client);
    return ret;
}
