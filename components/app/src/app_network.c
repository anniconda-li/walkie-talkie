/**
 * @file app_network.c
 * @brief 用户网络模式选择、NVS 记忆和 service 网络后端重装。
 */
#include "app_network.h"

#include "app_config.h"
#include "app_intercom.h"
#include "d_ml307c.h"
#include "d_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_init.h"
#include "wdriver_uart.h"

#include <string.h>

static const char *TAG = "app_network";

#define APP_NETWORK_NVS_NS             "app_net"
#define APP_NETWORK_NVS_MODE           "mode"
#define APP_NETWORK_NVS_WIFI_SSID      "wifi_ssid"
#define APP_NETWORK_NVS_WIFI_PASSWORD  "wifi_pwd"
#define APP_NETWORK_WIFI_CONNECT_MS    15000u
#define APP_NETWORK_4G_TIMEOUT_MS      5000u
#define APP_NETWORK_4G_SOCKET_ID       1u

static volatile int s_started = 0;
static app_network_mode_t s_mode = APP_NETWORK_MODE_NONE;
static osal_mutex_t s_lock = NULL;

static int app_network_lock(void)
{
    if (s_lock == NULL) {
        s_lock = osal_mutex_create();
    }
    return s_lock != NULL ? osal_mutex_lock(s_lock, OSAL_WAIT_FOREVER) : -1;
}

static void app_network_unlock(void)
{
    if (s_lock != NULL) {
        osal_mutex_unlock(s_lock);
    }
}

static d_ml307c_wdriver_ops_t app_network_ml307c_ops(void)
{
    d_ml307c_wdriver_ops_t ops = {
        .uart_write = wdriver_uart_write,
        .uart_read = wdriver_uart_read,
        .delay_ms = osal_delay_ms,
        .get_tick_ms = osal_get_tick_ms,
    };
    return ops;
}

static ml307c_config_t app_network_ml307c_cfg(void)
{
    ml307c_config_t cfg = {
        .timeout_ms = APP_NETWORK_4G_TIMEOUT_MS,
        .socket_id = APP_NETWORK_4G_SOCKET_ID,
    };
    return cfg;
}

static void app_network_set_mode(app_network_mode_t mode)
{
    if (app_network_lock() == 0) {
        s_mode = mode;
        app_network_unlock();
    }
}

static int app_network_nvs_open(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    if (handle == NULL) {
        return -1;
    }

    int ret = nvs_flash_init();
    if (ret != 0) {
        (void)nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != 0) {
        return -2;
    }

    return nvs_open(APP_NETWORK_NVS_NS, mode, handle) == 0 ? 0 : -3;
}

static void app_network_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (app_network_nvs_open(&handle, NVS_READWRITE) != 0) {
        return;
    }

    (void)nvs_set_u8(handle, APP_NETWORK_NVS_MODE, (uint8_t)APP_NETWORK_MODE_WIFI);
    (void)nvs_set_str(handle, APP_NETWORK_NVS_WIFI_SSID, ssid);
    (void)nvs_set_str(handle, APP_NETWORK_NVS_WIFI_PASSWORD, password != NULL ? password : "");
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static void app_network_save_4g(void)
{
    nvs_handle_t handle;
    if (app_network_nvs_open(&handle, NVS_READWRITE) != 0) {
        return;
    }

    (void)nvs_set_u8(handle, APP_NETWORK_NVS_MODE, (uint8_t)APP_NETWORK_MODE_4G);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static app_network_mode_t app_network_load_mode(void)
{
    nvs_handle_t handle;
    uint8_t mode = (uint8_t)APP_NETWORK_MODE_NONE;

    if (app_network_nvs_open(&handle, NVS_READONLY) != 0) {
        return APP_NETWORK_MODE_NONE;
    }

    (void)nvs_get_u8(handle, APP_NETWORK_NVS_MODE, &mode);
    nvs_close(handle);
    return (mode == APP_NETWORK_MODE_WIFI || mode == APP_NETWORK_MODE_4G)
               ? (app_network_mode_t)mode
               : APP_NETWORK_MODE_NONE;
}

static int app_network_load_wifi(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t handle;
    size_t len;

    if (ssid == NULL || password == NULL || ssid_size == 0u || password_size == 0u) {
        return -1;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    if (app_network_nvs_open(&handle, NVS_READONLY) != 0) {
        return -2;
    }

    len = ssid_size;
    int ret = nvs_get_str(handle, APP_NETWORK_NVS_WIFI_SSID, ssid, &len);
    if (ret == 0) {
        len = password_size;
        (void)nvs_get_str(handle, APP_NETWORK_NVS_WIFI_PASSWORD, password, &len);
    }
    nvs_close(handle);
    return (ret == 0 && ssid[0] != '\0') ? 0 : -3;
}

static app_network_4g_status_t app_network_map_4g_status(const d_ml307c_status_t *status)
{
    if (status == NULL || status->at_ready != 1) {
        return APP_NETWORK_4G_NO_AT;
    }
    if (status->sim_ready != 1) {
        return APP_NETWORK_4G_NO_SIM;
    }
    if (status->reg_state != 1 && status->reg_state != 5) {
        return APP_NETWORK_4G_NOT_REGISTERED;
    }
    if (status->link_state != 1) {
        return APP_NETWORK_4G_UNAVAILABLE;
    }
    return APP_NETWORK_4G_OK;
}

int app_network_scan_wifi(app_network_wifi_ap_t *items, size_t max, size_t *count)
{
    if (items == NULL || count == NULL || max == 0u || max > UINT16_MAX) {
        return -1;
    }

    d_wifi_ap_record_t records[max];
    uint16_t scan_count = 0;
    int ret = d_wifi_scan(records, (uint16_t)max, &scan_count);
    if (ret != 0) {
        *count = 0u;
        return ret;
    }

    for (uint16_t i = 0; i < scan_count; i++) {
        strncpy(items[i].ssid, records[i].ssid, sizeof(items[i].ssid) - 1u);
        items[i].ssid[sizeof(items[i].ssid) - 1u] = '\0';
        items[i].rssi = records[i].rssi;
        items[i].authmode = records[i].authmode;
    }
    *count = scan_count;
    return 0;
}

int app_network_connect_wifi(const char *ssid, const char *password)
{
    if (password == NULL || strlen(password) < 8u) {
        return -1;
    }

    int ret = d_wifi_connect(ssid, password, APP_NETWORK_WIFI_CONNECT_MS);
    if (ret != 0) {
        APP_LOGW(TAG, "WiFi 连接失败, ssid=%s, ret=%d", ssid != NULL ? ssid : "", ret);
        return ret;
    }

    ret = service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    if (ret != 0) {
        return ret;
    }

    app_network_save_wifi(ssid, password);
    app_network_set_mode(APP_NETWORK_MODE_WIFI);
    app_intercom_network_changed();
    APP_LOGI(TAG, "已切换到 WLAN");
    return 0;
}

int app_network_select_4g(app_network_4g_status_t *status)
{
    d_ml307c_wdriver_ops_t ops = app_network_ml307c_ops();
    ml307c_config_t cfg = app_network_ml307c_cfg();
    d_ml307c_status_t ml_status = {0};

    int ret = d_ml307c_prepare(&ops, &cfg);
    if (ret != 0) {
        if (status != NULL) {
            *status = APP_NETWORK_4G_NO_AT;
        }
        return ret;
    }

    ret = d_ml307c_probe(&ml_status);
    app_network_4g_status_t mapped = ret == 0 ? app_network_map_4g_status(&ml_status)
                                              : APP_NETWORK_4G_UNAVAILABLE;
    if (status != NULL) {
        *status = mapped;
    }
    if (mapped != APP_NETWORK_4G_OK) {
        return -2;
    }

    ret = service_init_network_for(SERVICE_NETWORK_BACKEND_4G);
    if (ret != 0) {
        return ret;
    }

    app_network_save_4g();
    app_network_set_mode(APP_NETWORK_MODE_4G);
    app_intercom_network_changed();
    APP_LOGI(TAG, "已切换到 4G");
    return 0;
}

int app_network_recover(void)
{
    app_network_mode_t mode = app_network_get_mode();

    if (mode == APP_NETWORK_MODE_4G) {
        app_network_4g_status_t status;
        return app_network_select_4g(&status);
    }
    if (mode == APP_NETWORK_MODE_WIFI || d_wifi_is_initialized() == 1) {
        return service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    }

    return -1;
}

app_network_mode_t app_network_get_mode(void)
{
    app_network_mode_t mode;
    if (app_network_lock() != 0) {
        return s_mode;
    }
    mode = s_mode;
    app_network_unlock();
    return mode;
}

int app_network_get_wifi_ssid(char *ssid, size_t size)
{
    d_wifi_status_t status;

    if (ssid == NULL || size == 0u) {
        return -1;
    }

    ssid[0] = '\0';
    if (d_wifi_get_status(&status) != 0 || status.link_ready != 1 || status.ssid[0] == '\0') {
        return -2;
    }

    strncpy(ssid, status.ssid, size - 1u);
    ssid[size - 1u] = '\0';
    return 0;
}

static void app_network_task(void *arg)
{
    (void)arg;

    int wifi_ret = d_wifi_prepare();
    if (wifi_ret != 0) {
        APP_LOGW(TAG, "WiFi 资源准备失败, ret=%d", wifi_ret);
    } else {
        (void)service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    }

    d_ml307c_wdriver_ops_t ops = app_network_ml307c_ops();
    ml307c_config_t cfg = app_network_ml307c_cfg();
    int ml_ret = d_ml307c_prepare(&ops, &cfg);
    if (ml_ret != 0) {
        APP_LOGW(TAG, "4G 资源准备失败, ret=%d", ml_ret);
    }

    app_network_mode_t saved_mode = app_network_load_mode();
    if (saved_mode == APP_NETWORK_MODE_WIFI) {
        char ssid[33];
        char password[65];
        if (app_network_load_wifi(ssid, sizeof(ssid), password, sizeof(password)) == 0 &&
            app_network_connect_wifi(ssid, password) != 0) {
            APP_LOGW(TAG, "开机自动连接 WLAN 失败");
        }
    } else if (saved_mode == APP_NETWORK_MODE_4G) {
        app_network_4g_status_t status;
        if (app_network_select_4g(&status) != 0) {
            APP_LOGW(TAG, "开机自动选择 4G 失败, status=%d", (int)status);
        }
    }

    while (1) {
        osal_delay_ms(60000u);
    }
}

int app_network_start(void)
{
    if (s_started) {
        return 0;
    }

    if (s_lock == NULL) {
        s_lock = osal_mutex_create();
        if (s_lock == NULL) {
            return -1;
        }
    }

    int ret = osal_task_create("app_network",
                               app_network_task,
                               NULL,
                               6144u,
                               4u,
                               NULL);
    if (ret != 0) {
        return ret;
    }

    s_started = 1;
    return 0;
}
