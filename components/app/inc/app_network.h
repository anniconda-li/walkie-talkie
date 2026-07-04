/**
 * @file app_network.h
 * @brief App 层网络模式选择和后台准备编排。
 */
#ifndef APP_NETWORK_H
#define APP_NETWORK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_NETWORK_MODE_NONE = 0,
    APP_NETWORK_MODE_WIFI,
    APP_NETWORK_MODE_4G,
} app_network_mode_t;

typedef struct {
    char ssid[33];
    int rssi;
    int authmode;
} app_network_wifi_ap_t;

int app_network_start(void);
int app_network_scan_wifi(app_network_wifi_ap_t *items, size_t max, size_t *count);
int app_network_connect_wifi(const char *ssid, const char *password);
int app_network_enter_wifi_scan_mode(void);
int app_network_select_4g(void);
int app_network_recover(void);
app_network_mode_t app_network_get_mode(void);
int app_network_get_wifi_ssid(char *ssid, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* APP_NETWORK_H */
