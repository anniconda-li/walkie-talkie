/**
 * @file driver_wifi.h
 * @brief WiFi STA network driver interface.
 */
#ifndef DRIVER_WIFI_H
#define DRIVER_WIFI_H

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
} driver_wifi_config_t;

/**
 * @brief WiFi network status.
 */
typedef struct {
    int rssi;       /**< RSSI mapped to CSQ-like 0-31 scale; 99 means unknown. */
    int link_ready; /**< 1 after STA got IP; otherwise 0. */
} driver_wifi_status_t;

int driver_wifi_init(const driver_wifi_config_t *cfg);
int driver_wifi_deinit(void);
int driver_wifi_is_initialized(void);
int driver_wifi_get_status(driver_wifi_status_t *status);
int driver_wifi_is_ready(void);
int driver_wifi_udp_connect(const char *host, int port);
int driver_wifi_udp_send(const uint8_t *data, int len);
int driver_wifi_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
int driver_wifi_tcp_connect(const char *host, int port);
int driver_wifi_tcp_send(const uint8_t *data, int len);
int driver_wifi_tcp_close(void);
int driver_wifi_http_post_wav(const char *url,
                              const uint8_t *wav,
                              uint16_t wav_len,
                              uint8_t *resp,
                              uint16_t resp_size,
                              uint16_t *resp_len,
                              uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_WIFI_H */
