/**
 * @file app_network.c
 * @brief 用户网络模式选择、NVS 记忆和 service 网络后端重装。
 */
#include "app_network.h"

#include "app_config.h"
#include "app_intercom.h"
#include "d_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_init.h"
#include "service_network.h"

#include <string.h>

static const char *TAG = "app_network";

#define APP_NETWORK_NVS_NS             "app_net"
#define APP_NETWORK_NVS_MODE           "mode"
#define APP_NETWORK_NVS_WIFI_SSID      "wifi_ssid"
#define APP_NETWORK_NVS_WIFI_PASSWORD  "wifi_pwd"
#define APP_NETWORK_WIFI_CONNECT_MS    12000u
#define APP_NETWORK_BOOT_WIFI_CONNECT_MS 6000u
#define APP_NETWORK_FAKE_4G_CONNECT_MS 22000u
#define APP_NETWORK_WIFI_RETRY_COUNT   2u
#define APP_NETWORK_BOOT_WIFI_RETRY_COUNT 1u
#define APP_NETWORK_FAKE_4G_RETRY_COUNT 5u
#define APP_NETWORK_RETRY_DELAY_MS     1200u
#define APP_NETWORK_MONITOR_MS         10000u
#define APP_NETWORK_CANCEL_WAIT_MS     1800u
#define APP_NETWORK_CANCEL_POLL_MS     50u
#define APP_NETWORK_FAKE_4G_SSID       "14"
#define APP_NETWORK_FAKE_4G_PASSWORD   "12345678"

static volatile int s_started = 0;
static volatile int s_switching = 0;
static volatile int s_auto_connecting = 0;
static volatile int s_auto_connect_cancel = 0;
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

static int app_network_begin_switch(void)
{
    if (app_network_lock() != 0) {
        return -1;
    }
    if (s_switching) {
        app_network_unlock();
        return -2;
    }
    s_switching = 1;
    app_network_unlock();
    return 0;
}

static void app_network_end_switch(void)
{
    if (app_network_lock() == 0) {
        s_switching = 0;
        app_network_unlock();
    }
}

static int app_network_cancel_auto_connect_and_wait(uint32_t wait_ms)
{
    if (!s_auto_connecting) {
        return 0;
    }

    s_auto_connect_cancel = 1;
    (void)d_wifi_cancel_connect();

    uint32_t start = osal_get_tick_ms();
    while (s_auto_connecting && (osal_get_tick_ms() - start) < wait_ms) {
        osal_delay_ms(APP_NETWORK_CANCEL_POLL_MS);
    }

    return s_auto_connecting ? -1 : 0;
}

static void app_network_set_mode(app_network_mode_t mode)
{
    if (app_network_lock() == 0) {
        s_mode = mode;
        app_network_unlock();
    }
}

static void app_network_enter_user_mode(app_network_mode_t mode)
{
    (void)service_network_deinit();
    app_network_set_mode(mode);
    app_intercom_network_changed();
}

static int app_network_ensure_wifi_service(void)
{
    if (service_network_init(NULL) == 0) {
        return 0;
    }

    return service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
}

static int app_network_connect_sta_with_retry(const char *ssid,
                                              const char *password,
                                              uint32_t timeout_ms,
                                              uint32_t retry_count,
                                              volatile int *cancel_flag)
{
    int ret = -1;

    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }

    for (uint32_t attempt = 0u; attempt < retry_count; attempt++) {
        if (cancel_flag != NULL && *cancel_flag) {
            return -7;
        }
        (void)d_wifi_disconnect();
        osal_delay_ms(300u);
        if (cancel_flag != NULL && *cancel_flag) {
            return -7;
        }
        ret = d_wifi_connect(ssid, password, timeout_ms);
        if (ret == 0) {
            return 0;
        }
        if (cancel_flag != NULL && *cancel_flag) {
            return -7;
        }

        APP_LOGW(TAG,
                 "WiFi STA 连接失败, ssid=%s, attempt=%u/%u, ret=%d",
                 ssid,
                 (unsigned int)(attempt + 1u),
                 (unsigned int)retry_count,
                 ret);
        osal_delay_ms(APP_NETWORK_RETRY_DELAY_MS);
    }

    return ret;
}

static int app_network_is_connected_to(const char *ssid)
{
    d_wifi_status_t status;

    if (ssid == NULL || ssid[0] == '\0') {
        return 0;
    }
    if (d_wifi_get_status(&status) != 0 || status.link_ready != 1) {
        return 0;
    }

    return strcmp(status.ssid, ssid) == 0 ? 1 : 0;
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

int app_network_scan_wifi(app_network_wifi_ap_t *items, size_t max, size_t *count)
{
    if (items == NULL || count == NULL || max == 0u || max > UINT16_MAX) {
        return -1;
    }
    (void)app_network_cancel_auto_connect_and_wait(APP_NETWORK_CANCEL_WAIT_MS);

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
    int ret;

    if (password == NULL || strlen(password) < 8u) {
        return -1;
    }

    (void)app_network_cancel_auto_connect_and_wait(APP_NETWORK_CANCEL_WAIT_MS);

    ret = app_network_begin_switch();
    if (ret != 0) {
        return ret;
    }

    app_network_enter_user_mode(APP_NETWORK_MODE_WIFI);

    ret = app_network_connect_sta_with_retry(ssid,
                                             password,
                                             APP_NETWORK_WIFI_CONNECT_MS,
                                             APP_NETWORK_WIFI_RETRY_COUNT,
                                             NULL);
    if (ret != 0) {
        APP_LOGW(TAG, "WiFi 连接失败, ssid=%s, ret=%d", ssid != NULL ? ssid : "", ret);
        (void)service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
        app_network_end_switch();
        return ret;
    }

    ret = service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    if (ret != 0) {
        app_network_end_switch();
        return ret;
    }

    app_network_save_wifi(ssid, password);
    app_intercom_network_changed();
    APP_LOGI(TAG, "已切换到 WLAN");
    app_network_end_switch();
    return 0;
}

int app_network_enter_wifi_scan_mode(void)
{
    (void)app_network_cancel_auto_connect_and_wait(APP_NETWORK_CANCEL_WAIT_MS);

    int ret = app_network_begin_switch();
    if (ret != 0) {
        return ret;
    }

    (void)d_wifi_disconnect();
    ret = service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    if (ret == 0) {
        app_network_set_mode(APP_NETWORK_MODE_WIFI);
        app_intercom_network_changed();
        APP_LOGI(TAG, "已切换到 WLAN 扫描模式");
    } else {
        APP_LOGW(TAG, "WLAN 扫描模式切换失败, ret=%d", ret);
    }

    app_network_end_switch();
    return ret;
}

int app_network_select_4g(void)
{
    (void)app_network_cancel_auto_connect_and_wait(APP_NETWORK_CANCEL_WAIT_MS);

    int ret = app_network_begin_switch();
    if (ret != 0) {
        return ret;
    }

    ret = app_network_ensure_wifi_service();
    if (ret != 0) {
        app_network_end_switch();
        return ret;
    }

    if (app_network_is_connected_to(APP_NETWORK_FAKE_4G_SSID)) {
        app_network_set_mode(APP_NETWORK_MODE_4G);
        app_intercom_network_changed();
        app_network_end_switch();
        APP_LOGI(TAG, "伪 4G WiFi 已连接, ssid=%s", APP_NETWORK_FAKE_4G_SSID);
        return 0;
    }

    (void)d_wifi_disconnect();

    ret = app_network_connect_sta_with_retry(APP_NETWORK_FAKE_4G_SSID,
                                             APP_NETWORK_FAKE_4G_PASSWORD,
                                             APP_NETWORK_FAKE_4G_CONNECT_MS,
                                             APP_NETWORK_FAKE_4G_RETRY_COUNT,
                                             NULL);
    if (ret != 0) {
        APP_LOGW(TAG, "伪 4G WiFi 连接失败, ssid=%s, ret=%d", APP_NETWORK_FAKE_4G_SSID, ret);
        (void)d_wifi_disconnect();
        app_network_end_switch();
        return ret;
    }

    app_network_set_mode(APP_NETWORK_MODE_4G);
    app_intercom_network_changed();
    app_network_end_switch();
    APP_LOGI(TAG, "已切换到伪 4G WiFi, ssid=%s", APP_NETWORK_FAKE_4G_SSID);
    return 0;
}

int app_network_recover(void)
{
    app_network_mode_t mode = app_network_get_mode();

    if (s_switching) {
        return -1;
    }

    if (mode == APP_NETWORK_MODE_4G) {
        return app_network_select_4g();
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

static int app_network_prepare_wifi_backend(void)
{
    int wifi_ret = d_wifi_prepare();
    if (wifi_ret != 0) {
        APP_LOGW(TAG, "WiFi 资源准备失败, ret=%d", wifi_ret);
        return wifi_ret;
    }

    APP_LOGI(TAG, "WiFi 后端准备成功");
    return 0;
}

static int app_network_prepare_boot_backend(void)
{
    int wifi_ret = app_network_prepare_wifi_backend();
    if (wifi_ret != 0) {
        APP_LOGE(TAG, "开机 WLAN 初始化失败, ret=%d", wifi_ret);
        return wifi_ret;
    }

    int ret = service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
    if (ret != 0) {
        APP_LOGE(TAG, "开机 WLAN 服务装配失败, ret=%d", ret);
        return ret;
    }

    app_network_set_mode(APP_NETWORK_MODE_WIFI);
    return 0;
}

static void app_network_task(void *arg)
{
    (void)arg;

    char ssid[33];
    char password[65];
    int load_ret = app_network_load_wifi(ssid, sizeof(ssid), password, sizeof(password));
    if (load_ret == 0) {
        s_auto_connecting = 1;
        s_auto_connect_cancel = 0;
        int ret = app_network_begin_switch();
        if (ret == 0) {
            ret = app_network_connect_sta_with_retry(ssid,
                                                     password,
                                                     APP_NETWORK_BOOT_WIFI_CONNECT_MS,
                                                     APP_NETWORK_BOOT_WIFI_RETRY_COUNT,
                                                     &s_auto_connect_cancel);
            if (ret == 0 && !s_auto_connect_cancel) {
                ret = service_init_network_for(SERVICE_NETWORK_BACKEND_WIFI);
                if (ret == 0) {
                    app_network_set_mode(APP_NETWORK_MODE_WIFI);
                    app_intercom_network_changed();
                    APP_LOGI(TAG, "开机自动连接上次 WLAN 成功, ssid=%s", ssid);
                }
            }
            app_network_end_switch();
        }
        if (s_auto_connect_cancel) {
            APP_LOGI(TAG, "开机自动连接已被用户操作取消");
        } else if (ret != 0) {
            APP_LOGW(TAG, "开机自动连接上次 WLAN 失败, ssid=%s, ret=%d", ssid, ret);
        }
        s_auto_connecting = 0;
    } else {
        APP_LOGI(TAG, "开机未找到已保存 WLAN，等待用户扫描选择");
    }

    while (1) {
        osal_delay_ms(APP_NETWORK_MONITOR_MS);
        if (s_switching) {
            continue;
        }

        app_network_mode_t mode = app_network_get_mode();
        if (mode == APP_NETWORK_MODE_4G && service_network_is_ready() != 1) {
            APP_LOGW(TAG, "网络未就绪，按当前模式恢复, mode=%d", (int)mode);
            (void)app_network_recover();
        }
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

    int ret = app_network_prepare_boot_backend();
    if (ret != 0) {
        return ret;
    }

    ret = osal_task_create("app_network",
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
