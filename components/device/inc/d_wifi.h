/**
 * @file d_wifi.h
 * @brief WiFi STA network driver interface.
 */
#ifndef D_WIFI_H
#define D_WIFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi STA initialization config.
 */
typedef struct {
    const char *ssid;     /**< WiFi SSID. */
    const char *password; /**< WiFi password. */
} d_wifi_config_t;

/**
 * @brief WiFi network status.
 */
typedef struct {
    int rssi;       /**< RSSI mapped to CSQ-like 0-31 scale; 99 means unknown. */
    int link_ready; /**< 1 after STA got IP; otherwise 0. */
} d_wifi_status_t;

int d_wifi_init(const d_wifi_config_t *cfg);
int d_wifi_deinit(void);
int d_wifi_is_initialized(void);
int d_wifi_get_status(d_wifi_status_t *status);
int d_wifi_is_ready(void);
int d_wifi_udp_connect(const char *host, int port);
int d_wifi_udp_send(const uint8_t *data, int len);
int d_wifi_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
int d_wifi_tcp_connect(const char *host, int port);
int d_wifi_tcp_send(const uint8_t *data, int len);
int d_wifi_tcp_close(void);
int d_wifi_http_post(const char *url,
                          const char *content_type,
                          const uint8_t *body,
                          uint32_t body_len,
                          uint8_t *resp,
                          uint32_t resp_size,
                          uint32_t *resp_len,
                          uint32_t timeout_ms);
int d_wifi_http_post_wav(const char *url,
                              const uint8_t *wav,
                              uint16_t wav_len,
                              uint8_t *resp,
                              uint16_t resp_size,
                              uint16_t *resp_len,
                              uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* D_WIFI_H */
