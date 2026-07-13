/**
 * @file app_ota.c
 * @brief 仅 WiFi/HTTP 的双分区 OTA 实现。
 */
#include "app_ota.h"

#include "app_business.h"
#include "app_config.h"
#include "app_ui.h"
#include "osal_task.h"
#include "service_battery.h"
#include "service_network.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_ota";

#define APP_OTA_TASK_STACK             16384u
#define APP_OTA_TASK_PRIORITY          3u
#define APP_OTA_CHECK_DELAY_MS         30000u
#define APP_OTA_CHECK_INTERVAL_MS      (6u * 60u * 60u * 1000u)
#define APP_OTA_REPORT_RETRY_MS        60000u
#define APP_OTA_HTTP_TIMEOUT_MS        30000u
#define APP_OTA_CHECK_RESPONSE_BYTES   2048u
#define APP_OTA_DOWNLOAD_BUFFER_BYTES  4096u
#define APP_OTA_BOOT_GUARD_MS          60000u
#define APP_OTA_PROGRESS_STEP_MS       250u
#define APP_OTA_NVS_NS                 "app_ota"
#define APP_OTA_CACHE_KEY              "update"
#define APP_OTA_REPORT_KEY             "report"
#define APP_OTA_ATTEMPT_KEY            "attempt"
#define APP_OTA_RECORD_MAGIC           0x4f544131u

typedef struct {
    uint32_t magic;
    uint8_t available;
    uint8_t mandatory;
    uint8_t min_battery;
    uint8_t reserved;
    uint32_t size;
    char version[24];
    char sha256[65];
    char firmware_url[224];
    char release_notes[256];
} app_ota_update_info_t;

typedef struct {
    uint32_t magic;
    uint32_t bytes_written;
    char from_version[24];
    char to_version[24];
    char status[24];
    char error_code[40];
    char error_message[96];
} app_ota_report_t;

typedef struct {
    uint32_t magic;
    uint32_t bytes_written;
    char from_version[24];
    char to_version[24];
} app_ota_attempt_t;

typedef struct {
    char content_type[64];
} app_ota_http_headers_t;

static osal_task_t s_ota_task = NULL;
static osal_task_t s_boot_guard_task = NULL;
static volatile app_ota_state_t s_state = APP_OTA_STATE_IDLE;
static volatile int s_install_requested = 0;
static volatile int s_cancel_requested = 0;
static volatile int s_maintenance = 0;
static volatile int s_pending_verify = 0;
static app_ota_update_info_t s_update;
static app_ota_report_t s_pending_report;
static app_ota_attempt_t s_attempt;

static const char *app_ota_current_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc != NULL && desc->version[0] != '\0' ? desc->version : "0.0.0";
}

static int app_ota_nvs_open(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    if (handle == NULL) {
        return -1;
    }
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        return -2;
    }
    return nvs_open(APP_OTA_NVS_NS, mode, handle) == ESP_OK ? 0 : -3;
}

static void app_ota_save_blob(const char *key, const void *value, size_t size)
{
    nvs_handle_t handle;
    if (key == NULL || value == NULL || app_ota_nvs_open(&handle, NVS_READWRITE) != 0) {
        return;
    }
    if (nvs_set_blob(handle, key, value, size) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static int app_ota_load_blob(const char *key, void *value, size_t size)
{
    nvs_handle_t handle;
    if (key == NULL || value == NULL || app_ota_nvs_open(&handle, NVS_READONLY) != 0) {
        return -1;
    }
    size_t actual = size;
    int ret = nvs_get_blob(handle, key, value, &actual) == ESP_OK && actual == size ? 0 : -2;
    nvs_close(handle);
    return ret;
}

static void app_ota_erase_key(const char *key)
{
    nvs_handle_t handle;
    if (key == NULL || app_ota_nvs_open(&handle, NVS_READWRITE) != 0) {
        return;
    }
    if (nvs_erase_key(handle, key) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static int app_ota_parse_semver(const char *version, uint32_t out[3])
{
    if (version == NULL || out == NULL) {
        return -1;
    }
    char tail = '\0';
    unsigned int major = 0u;
    unsigned int minor = 0u;
    unsigned int patch = 0u;
    if (sscanf(version, "%u.%u.%u%c", &major, &minor, &patch, &tail) != 3) {
        return -2;
    }
    out[0] = major;
    out[1] = minor;
    out[2] = patch;
    return 0;
}

static int app_ota_version_is_newer(const char *candidate, const char *current)
{
    uint32_t lhs[3];
    uint32_t rhs[3];
    if (app_ota_parse_semver(candidate, lhs) != 0 || app_ota_parse_semver(current, rhs) != 0) {
        return 0;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        if (lhs[i] != rhs[i]) {
            return lhs[i] > rhs[i] ? 1 : 0;
        }
    }
    return 0;
}

static int app_ota_sha_is_valid(const char *sha)
{
    if (sha == NULL || strlen(sha) != 64u) {
        return 0;
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        if (!isdigit((unsigned char)sha[i]) && (sha[i] < 'a' || sha[i] > 'f')) {
            return 0;
        }
    }
    return 1;
}

static int app_ota_hex_to_bytes(const char *hex, uint8_t out[32])
{
    if (!app_ota_sha_is_valid(hex) || out == NULL) {
        return -1;
    }
    for (uint32_t i = 0u; i < 32u; i++) {
        unsigned int value = 0u;
        if (sscanf(&hex[i * 2u], "%2x", &value) != 1) {
            return -2;
        }
        out[i] = (uint8_t)value;
    }
    return 0;
}

static esp_err_t app_ota_http_event(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) {
        return ESP_OK;
    }
    app_ota_http_headers_t *headers = (app_ota_http_headers_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        snprintf(headers->content_type, sizeof(headers->content_type), "%s", event->header_value);
    }
    return ESP_OK;
}

static int app_ota_http_get_small(const char *url, char *response, size_t response_size)
{
    if (url == NULL || response == NULL || response_size < 2u) {
        return -1;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -2;
    }
    int ret = -3;
    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (esp_http_client_open(client, 0) == ESP_OK) {
        int64_t content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (content_len > 0 && content_len < (int64_t)response_size && status == 200 &&
            !esp_http_client_is_chunked_response(client)) {
            int read_len = esp_http_client_read_response(client, response, (int)response_size - 1);
            if (read_len >= 0 && (content_len == 0 || read_len == content_len)) {
                response[read_len] = '\0';
                ret = read_len;
            }
        }
    }
    esp_http_client_cleanup(client);
    return ret;
}

static void app_ota_apply_update_to_ui(void)
{
    (void)app_ui_set_ota_update(s_update.available,
                                s_update.version,
                                s_update.size,
                                s_update.min_battery,
                                s_update.mandatory,
                                s_update.release_notes);
}

static void app_ota_save_update(void)
{
    s_update.magic = APP_OTA_RECORD_MAGIC;
    app_ota_save_blob(APP_OTA_CACHE_KEY, &s_update, sizeof(s_update));
    app_ota_apply_update_to_ui();
}

static int app_ota_parse_check_response(const char *json, app_ota_update_info_t *info)
{
    if (json == NULL || info == NULL) {
        return -1;
    }
    cJSON *root = cJSON_Parse(json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -2;
    }
    cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "update");
    if (!cJSON_IsBool(update)) {
        cJSON_Delete(root);
        return -3;
    }
    memset(info, 0, sizeof(*info));
    info->magic = APP_OTA_RECORD_MAGIC;
    if (!cJSON_IsTrue(update)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *hardware = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "firmware_url");
    cJSON *mandatory = cJSON_GetObjectItemCaseSensitive(root, "mandatory");
    cJSON *min_battery = cJSON_GetObjectItemCaseSensitive(root, "min_battery");
    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "release_notes");
    if (!cJSON_IsString(version) || !cJSON_IsString(hardware) || !cJSON_IsString(channel) ||
        !cJSON_IsNumber(size) || !cJSON_IsString(sha) || !cJSON_IsString(url) ||
        !cJSON_IsBool(mandatory) || !cJSON_IsNumber(min_battery) || !cJSON_IsString(notes) ||
        strcmp(hardware->valuestring, APP_OTA_HARDWARE) != 0 ||
        strcmp(channel->valuestring, APP_OTA_CHANNEL) != 0 ||
        size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
        size->valuedouble != (double)(uint32_t)size->valuedouble ||
        min_battery->valuedouble < 0 || min_battery->valuedouble > 100 ||
        min_battery->valuedouble != (double)(uint8_t)min_battery->valuedouble ||
        strlen(version->valuestring) >= sizeof(info->version) ||
        strlen(url->valuestring) >= sizeof(info->firmware_url) ||
        !app_ota_sha_is_valid(sha->valuestring) ||
        strncmp(url->valuestring, APP_OTA_BASE_URL "/", strlen(APP_OTA_BASE_URL) + 1u) != 0 ||
        app_ota_parse_semver(version->valuestring, (uint32_t[3]){0}) != 0) {
        cJSON_Delete(root);
        return -4;
    }

    if (!app_ota_version_is_newer(version->valuestring, app_ota_current_version())) {
        cJSON_Delete(root);
        return 0;
    }
    info->available = 1u;
    info->size = (uint32_t)size->valuedouble;
    snprintf(info->version, sizeof(info->version), "%s", version->valuestring);
    snprintf(info->sha256, sizeof(info->sha256), "%s", sha->valuestring);
    snprintf(info->firmware_url, sizeof(info->firmware_url), "%s", url->valuestring);
    info->mandatory = cJSON_IsTrue(mandatory) ? 1u : 0u;
    info->min_battery = (uint8_t)min_battery->valueint;
    snprintf(info->release_notes, sizeof(info->release_notes), "%s", notes->valuestring);
    cJSON_Delete(root);
    return 1;
}

static int app_ota_check_now(void)
{
    char url[384];
    char response[APP_OTA_CHECK_RESPONSE_BYTES];
    int written = snprintf(url,
                           sizeof(url),
                           APP_OTA_BASE_URL "/api/v1/ota/check?device_id=%s&hardware=%s&current_version=%s&network=wifi&channel=%s",
                           APP_DEVICE_ID,
                           APP_OTA_HARDWARE,
                           app_ota_current_version(),
                           APP_OTA_CHANNEL);
    if (written <= 0 || (size_t)written >= sizeof(url)) {
        return -1;
    }
    int response_len = app_ota_http_get_small(url, response, sizeof(response));
    if (response_len <= 0) {
        return -2;
    }
    app_ota_update_info_t parsed;
    int ret = app_ota_parse_check_response(response, &parsed);
    if (ret < 0) {
        return ret;
    }
    s_update = parsed;
    app_ota_save_update();
    s_state = parsed.available ? APP_OTA_STATE_AVAILABLE : APP_OTA_STATE_IDLE;
    APP_LOGI(TAG,
             "ota_check update=%u current=%s target=%s size=%u",
             (unsigned int)parsed.available,
             app_ota_current_version(),
             parsed.available ? parsed.version : "-",
             (unsigned int)parsed.size);
    return 0;
}

static void app_ota_queue_report(const char *status,
                                 const char *from_version,
                                 const char *to_version,
                                 uint32_t bytes_written,
                                 const char *error_code,
                                 const char *error_message)
{
    memset(&s_pending_report, 0, sizeof(s_pending_report));
    s_pending_report.magic = APP_OTA_RECORD_MAGIC;
    s_pending_report.bytes_written = bytes_written;
    snprintf(s_pending_report.status, sizeof(s_pending_report.status), "%s", status != NULL ? status : "failed");
    snprintf(s_pending_report.from_version, sizeof(s_pending_report.from_version), "%s", from_version != NULL ? from_version : app_ota_current_version());
    snprintf(s_pending_report.to_version, sizeof(s_pending_report.to_version), "%s", to_version != NULL ? to_version : "");
    snprintf(s_pending_report.error_code, sizeof(s_pending_report.error_code), "%s", error_code != NULL ? error_code : "");
    snprintf(s_pending_report.error_message, sizeof(s_pending_report.error_message), "%s", error_message != NULL ? error_message : "");
    app_ota_save_blob(APP_OTA_REPORT_KEY, &s_pending_report, sizeof(s_pending_report));
}

static int app_ota_try_report(void)
{
    if (s_pending_report.magic != APP_OTA_RECORD_MAGIC || service_network_is_ready() != 1) {
        return -1;
    }
    char url[128];
    uint8_t response[192];
    uint32_t response_len = 0u;
    snprintf(url, sizeof(url), APP_OTA_BASE_URL "/api/v1/ota/report");
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -2;
    }
    cJSON_AddStringToObject(root, "device_id", APP_DEVICE_ID);
    cJSON_AddStringToObject(root, "hardware", APP_OTA_HARDWARE);
    cJSON_AddStringToObject(root, "from_version", s_pending_report.from_version);
    cJSON_AddStringToObject(root, "to_version", s_pending_report.to_version);
    cJSON_AddStringToObject(root, "network", "wifi");
    cJSON_AddStringToObject(root, "status", s_pending_report.status);
    cJSON_AddNumberToObject(root, "bytes_written", s_pending_report.bytes_written);
    if (s_pending_report.error_code[0] != '\0') {
        cJSON_AddStringToObject(root, "error_code", s_pending_report.error_code);
        cJSON_AddStringToObject(root, "error_message", s_pending_report.error_message);
    } else {
        cJSON_AddNullToObject(root, "error_code");
        cJSON_AddNullToObject(root, "error_message");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -3;
    }
    int ret = service_network_http_post(url,
                                        "application/json",
                                        (const uint8_t *)json,
                                        (uint32_t)strlen(json),
                                        response,
                                        sizeof(response),
                                        &response_len,
                                        10000u);
    cJSON_free(json);
    if (ret == 0) {
        memset(&s_pending_report, 0, sizeof(s_pending_report));
        app_ota_erase_key(APP_OTA_REPORT_KEY);
    }
    return ret;
}

static void app_ota_save_attempt(const app_ota_update_info_t *info, uint32_t bytes)
{
    memset(&s_attempt, 0, sizeof(s_attempt));
    s_attempt.magic = APP_OTA_RECORD_MAGIC;
    s_attempt.bytes_written = bytes;
    snprintf(s_attempt.from_version, sizeof(s_attempt.from_version), "%s", app_ota_current_version());
    snprintf(s_attempt.to_version, sizeof(s_attempt.to_version), "%s", info->version);
    app_ota_save_blob(APP_OTA_ATTEMPT_KEY, &s_attempt, sizeof(s_attempt));
}

static void app_ota_clear_attempt(void)
{
    memset(&s_attempt, 0, sizeof(s_attempt));
    app_ota_erase_key(APP_OTA_ATTEMPT_KEY);
}

static int app_ota_download(const app_ota_update_info_t *info,
                            uint32_t *bytes_written,
                            const char **error_code,
                            const char **error_message)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || info == NULL || info->size == 0u || info->size > target->size) {
        *error_code = "PARTITION_SIZE";
        *error_message = "固件超过OTA分区容量";
        return -1;
    }

    app_ota_http_headers_t headers = {0};
    esp_http_client_config_t cfg = {
        .url = info->firmware_url,
        .timeout_ms = APP_OTA_HTTP_TIMEOUT_MS,
        .event_handler = app_ota_http_event,
        .user_data = &headers,
        .buffer_size = APP_OTA_DOWNLOAD_BUFFER_BYTES,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        *error_code = "HTTP_INIT";
        *error_message = "下载连接初始化失败";
        return -2;
    }

    esp_ota_handle_t ota_handle = 0;
    int ota_started = 0;
    int ota_ended = 0;
    int ret = -3;
    uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_BYTES);
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    if (buffer == NULL) {
        *error_code = "NO_MEMORY";
        *error_message = "升级缓冲区分配失败";
        goto cleanup;
    }

    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        *error_code = "HTTP_OPEN";
        *error_message = "无法连接固件服务器";
        goto cleanup;
    }
    int64_t content_len = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 ||
        esp_http_client_is_chunked_response(client) ||
        content_len != info->size ||
        (headers.content_type[0] != '\0' && strstr(headers.content_type, "application/octet-stream") == NULL)) {
        *error_code = "HTTP_HEADERS";
        *error_message = "固件响应头不符合约定";
        goto cleanup;
    }
    uint32_t prepare_start_ms = osal_get_tick_ms();
    APP_LOGI(TAG,
             "ota_download event=partition_prepare mode=sequential target=%s size=%u",
             target->label,
             (unsigned int)info->size);
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        *error_code = "OTA_BEGIN";
        *error_message = "无法准备升级分区";
        goto cleanup;
    }
    ota_started = 1;
    if (mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
        *error_code = "SHA_INIT";
        *error_message = "固件校验初始化失败";
        goto cleanup;
    }

    s_state = APP_OTA_STATE_DOWNLOADING;
    (void)app_ui_ota_show("正在升级", 0, 1);
    APP_LOGI(TAG,
             "ota_download event=partition_ready mode=sequential elapsed_ms=%u",
             (unsigned int)(osal_get_tick_ms() - prepare_start_ms));
    app_ota_queue_report("download_started", app_ota_current_version(), info->version, 0u, NULL, NULL);
    (void)app_ota_try_report();
    uint32_t last_progress_ms = 0u;
    while (*bytes_written < info->size) {
        if (s_cancel_requested != 0) {
            *error_code = "USER_CANCELLED";
            *error_message = "用户停止升级";
            ret = -100;
            goto cleanup;
        }
        uint32_t remain = info->size - *bytes_written;
        int want = (int)(remain > APP_OTA_DOWNLOAD_BUFFER_BYTES ? APP_OTA_DOWNLOAD_BUFFER_BYTES : remain);
        int got = esp_http_client_read(client, (char *)buffer, want);
        if (got <= 0) {
            *error_code = "DOWNLOAD_INTERRUPTED";
            *error_message = "固件下载中断";
            goto cleanup;
        }
        if (esp_ota_write(ota_handle, buffer, (size_t)got) != ESP_OK ||
            mbedtls_sha256_update(&sha_ctx, buffer, (size_t)got) != 0) {
            *error_code = "FLASH_WRITE";
            *error_message = "固件写入失败";
            goto cleanup;
        }
        *bytes_written += (uint32_t)got;
        osal_delay_ms(1u);
        uint32_t now = osal_get_tick_ms();
        if ((uint32_t)(now - last_progress_ms) >= APP_OTA_PROGRESS_STEP_MS || *bytes_written == info->size) {
            int percent = (int)((uint64_t)(*bytes_written) * 100u / info->size);
            (void)app_ui_ota_show("正在升级", percent, 1);
            last_progress_ms = now;
        }
    }

    s_state = APP_OTA_STATE_VERIFYING;
    (void)app_ui_ota_show("正在校验固件，请勿断电", 100, 0);
    uint8_t actual_sha[32];
    uint8_t expected_sha[32];
    if (mbedtls_sha256_finish(&sha_ctx, actual_sha) != 0 ||
        app_ota_hex_to_bytes(info->sha256, expected_sha) != 0 ||
        memcmp(actual_sha, expected_sha, sizeof(actual_sha)) != 0) {
        *error_code = "SHA256_MISMATCH";
        *error_message = "固件完整性校验失败";
        goto cleanup;
    }
    if (esp_ota_end(ota_handle) != ESP_OK) {
        ota_started = 0;
        *error_code = "IMAGE_INVALID";
        *error_message = "固件镜像校验失败";
        goto cleanup;
    }
    ota_ended = 1;
    ota_started = 0;

    esp_app_desc_t target_desc;
    if (esp_ota_get_partition_description(target, &target_desc) != ESP_OK ||
        strcmp(target_desc.version, info->version) != 0) {
        *error_code = "VERSION_MISMATCH";
        *error_message = "固件内部版本不匹配";
        goto cleanup;
    }
    app_ota_queue_report("verified", app_ota_current_version(), info->version, *bytes_written, NULL, NULL);
    (void)app_ota_try_report();
    app_ota_save_attempt(info, *bytes_written);

    s_state = APP_OTA_STATE_COMMITTING;
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        *error_code = "SET_BOOT";
        *error_message = "无法设置新启动分区";
        goto cleanup;
    }
    app_ota_queue_report("rebooting", app_ota_current_version(), info->version, *bytes_written, NULL, NULL);
    (void)app_ota_try_report();
    ret = 0;

cleanup:
    if (ota_started && !ota_ended) {
        (void)esp_ota_abort(ota_handle);
    }
    mbedtls_sha256_free(&sha_ctx);
    free(buffer);
    esp_http_client_cleanup(client);
    return ret;
}

static void app_ota_restore_after_failure(const char *message, int canceled)
{
    s_state = canceled ? APP_OTA_STATE_STOPPING : APP_OTA_STATE_FAILED;
    (void)app_ui_ota_show(canceled ? "升级已停止" : "升级未完成", -1, 0);
    osal_delay_ms(1000u);
    s_state = APP_OTA_STATE_RECOVERING;
    (void)app_ui_ota_show("正在恢复设备功能", -1, 0);
    app_business_exit_ota_mode();
    osal_delay_ms(800u);
    (void)app_ui_ota_finish_recovery(message);
    s_maintenance = 0;
    s_cancel_requested = 0;
    s_state = s_update.available ? APP_OTA_STATE_AVAILABLE : APP_OTA_STATE_IDLE;
}

static void app_ota_run_install(void)
{
    app_ota_update_info_t info = s_update;
    const char *error_code = "UNKNOWN";
    const char *error_message = "升级失败";
    uint32_t bytes_written = 0u;
    int canceled = 0;

    s_install_requested = 0;
    s_cancel_requested = 0;
    s_maintenance = 1;
    s_state = APP_OTA_STATE_PREPARING;
    (void)app_ui_ota_show("正在准备升级", -1, 1);

    if (service_network_is_ready() != 1) {
        error_code = "NO_WIFI";
        error_message = "WiFi未连接";
        goto failed;
    }
    int battery = 0;
    if (service_battery_get(&battery) != 0 || battery < info.min_battery) {
        error_code = "LOW_BATTERY";
        error_message = "当前电量不足";
        goto failed;
    }
    if (app_business_enter_ota_mode(30000u) != 0) {
        if (s_cancel_requested != 0) {
            error_code = "USER_CANCELLED";
            error_message = "用户停止升级";
            canceled = 1;
        } else {
            error_code = "BUSINESS_BUSY";
            error_message = "设备暂时无法进入升级状态";
        }
        goto failed;
    }
    if (s_cancel_requested != 0) {
        error_code = "USER_CANCELLED";
        error_message = "用户停止升级";
        canceled = 1;
        goto failed;
    }
    if (app_ota_check_now() != 0 || !s_update.available || strcmp(s_update.version, info.version) != 0) {
        error_code = "UPDATE_CHANGED";
        error_message = "升级版本已撤回或发生变化";
        goto failed;
    }
    info = s_update;
    if (app_ota_download(&info, &bytes_written, &error_code, &error_message) != 0) {
        canceled = error_code != NULL && strcmp(error_code, "USER_CANCELLED") == 0;
        goto failed;
    }

    s_state = APP_OTA_STATE_REBOOTING;
    (void)app_ui_ota_show("升级完成，正在重启", 100, 0);
    osal_delay_ms(1000u);
    esp_restart();
    return;

failed:
    app_ota_clear_attempt();
    app_ota_queue_report("failed",
                         app_ota_current_version(),
                         info.version,
                         bytes_written,
                         error_code,
                         error_message);
    (void)app_ota_try_report();
    app_ota_restore_after_failure(error_message, canceled);
}

static uint32_t app_ota_failure_retry_ms(uint8_t failures)
{
    if (failures <= 1u) {
        return 15u * 60u * 1000u;
    }
    if (failures == 2u) {
        return 30u * 60u * 1000u;
    }
    if (failures == 3u) {
        return 60u * 60u * 1000u;
    }
    return APP_OTA_CHECK_INTERVAL_MS;
}

static void app_ota_task(void *arg)
{
    (void)arg;
    uint32_t next_check_ms = 0u;
    uint32_t next_report_ms = 0u;
    uint8_t check_failures = 0u;
    uint8_t network_was_ready = 0u;

    while (1) {
        if (s_pending_verify != 0) {
            (void)osal_task_notify_take(1000u);
            continue;
        }
        uint32_t now = osal_get_tick_ms();
        int network_ready = service_network_is_ready() == 1;
        if (network_ready && !network_was_ready &&
            (next_check_ms == 0u || (int32_t)(now - next_check_ms) >= 0)) {
            next_check_ms = now + APP_OTA_CHECK_DELAY_MS;
            APP_LOGI(TAG,
                     "ota_check event=scheduled delay_ms=%u",
                     (unsigned int)APP_OTA_CHECK_DELAY_MS);
        }
        network_was_ready = network_ready ? 1u : 0u;

        if (s_install_requested != 0 && s_maintenance == 0 && s_update.available) {
            app_ota_run_install();
            next_check_ms = osal_get_tick_ms() + APP_OTA_CHECK_INTERVAL_MS;
        } else if (network_ready && s_maintenance == 0 && next_check_ms != 0u &&
                   (int32_t)(now - next_check_ms) >= 0) {
            s_state = APP_OTA_STATE_CHECKING;
            if (app_ota_check_now() == 0) {
                check_failures = 0u;
                next_check_ms = osal_get_tick_ms() + APP_OTA_CHECK_INTERVAL_MS;
            } else {
                check_failures++;
                s_state = s_update.available ? APP_OTA_STATE_AVAILABLE : APP_OTA_STATE_IDLE;
                next_check_ms = osal_get_tick_ms() + app_ota_failure_retry_ms(check_failures);
            }
        }

        now = osal_get_tick_ms();
        if (network_ready && s_maintenance == 0 && s_pending_report.magic == APP_OTA_RECORD_MAGIC &&
            (next_report_ms == 0u || (int32_t)(now - next_report_ms) >= 0)) {
            (void)app_ota_try_report();
            next_report_ms = osal_get_tick_ms() + APP_OTA_REPORT_RETRY_MS;
        }
        (void)osal_task_notify_take(1000u);
    }
}

static void app_ota_boot_guard_task(void *arg)
{
    (void)arg;
    (void)osal_task_notify_take(APP_OTA_BOOT_GUARD_MS);
    if (s_pending_verify != 0) {
        APP_LOGE(TAG, "ota_boot event=verify_timeout action=rollback");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    s_boot_guard_task = NULL;
    osal_task_delete_current();
}

int app_ota_boot_guard_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return 0;
    }
    s_pending_verify = 1;
    APP_LOGW(TAG,
             "ota_boot event=pending_verify guard_ms=%u",
             (unsigned int)APP_OTA_BOOT_GUARD_MS);
    return osal_task_create("ota_guard",
                            app_ota_boot_guard_task,
                            NULL,
                            4096u,
                            7u,
                            &s_boot_guard_task);
}

int app_ota_boot_confirm(void)
{
    if (s_pending_verify == 0) {
        return 0;
    }
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret != ESP_OK) {
        return -1;
    }
    s_pending_verify = 0;
    if (s_attempt.magic == APP_OTA_RECORD_MAGIC) {
        app_ota_queue_report("success",
                             s_attempt.from_version,
                             s_attempt.to_version,
                             s_attempt.bytes_written,
                             NULL,
                             NULL);
        memset(&s_attempt, 0, sizeof(s_attempt));
        app_ota_erase_key(APP_OTA_ATTEMPT_KEY);
    }
    if (s_ota_task != NULL) {
        (void)osal_task_notify_give(s_ota_task);
    }
    if (s_boot_guard_task != NULL) {
        (void)osal_task_notify_give(s_boot_guard_task);
    }
    APP_LOGI(TAG, "ota_boot event=confirmed version=%s", app_ota_current_version());
    return 0;
}

static void app_ota_detect_previous_rollback(void)
{
    if (s_attempt.magic != APP_OTA_RECORD_MAGIC) {
        return;
    }
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (invalid != NULL &&
        esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK &&
        strcmp(invalid_desc.version, s_attempt.to_version) == 0 &&
        strcmp(app_ota_current_version(), s_attempt.from_version) == 0) {
        app_ota_queue_report("rolled_back",
                             s_attempt.from_version,
                             s_attempt.to_version,
                             s_attempt.bytes_written,
                             "BOOT_ROLLBACK",
                             "新固件启动自检失败");
        memset(&s_attempt, 0, sizeof(s_attempt));
        app_ota_erase_key(APP_OTA_ATTEMPT_KEY);
    }
}

int app_ota_start(void)
{
    if (s_ota_task != NULL) {
        return 0;
    }
    if (app_ota_load_blob(APP_OTA_CACHE_KEY, &s_update, sizeof(s_update)) != 0 ||
        s_update.magic != APP_OTA_RECORD_MAGIC) {
        memset(&s_update, 0, sizeof(s_update));
    }
    if (s_update.available && !app_ota_version_is_newer(s_update.version, app_ota_current_version())) {
        memset(&s_update, 0, sizeof(s_update));
        app_ota_save_update();
    }
    if (app_ota_load_blob(APP_OTA_REPORT_KEY, &s_pending_report, sizeof(s_pending_report)) != 0 ||
        s_pending_report.magic != APP_OTA_RECORD_MAGIC) {
        memset(&s_pending_report, 0, sizeof(s_pending_report));
    }
    if (app_ota_load_blob(APP_OTA_ATTEMPT_KEY, &s_attempt, sizeof(s_attempt)) != 0 ||
        s_attempt.magic != APP_OTA_RECORD_MAGIC) {
        memset(&s_attempt, 0, sizeof(s_attempt));
    }
    app_ota_detect_previous_rollback();
    app_ota_apply_update_to_ui();
    s_state = s_update.available ? APP_OTA_STATE_AVAILABLE : APP_OTA_STATE_IDLE;
    TaskHandle_t handle = NULL;
    /* NVS 和 OTA 写 Flash 时会关闭 PSRAM 缓存，任务栈必须位于内部 SRAM。 */
    BaseType_t ret = xTaskCreateWithCaps(app_ota_task,
                                         "biz_ota",
                                         APP_OTA_TASK_STACK,
                                         NULL,
                                         APP_OTA_TASK_PRIORITY,
                                         &handle,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        return -1;
    }
    s_ota_task = (osal_task_t)handle;
    return 0;
}

int app_ota_request_install(void)
{
    if (s_ota_task == NULL || !s_update.available || s_maintenance != 0) {
        return -1;
    }
    s_install_requested = 1;
    (void)osal_task_notify_give(s_ota_task);
    return 0;
}

int app_ota_request_cancel(void)
{
    if (s_maintenance == 0 ||
        (s_state != APP_OTA_STATE_PREPARING && s_state != APP_OTA_STATE_DOWNLOADING)) {
        return -1;
    }
    s_cancel_requested = 1;
    return 0;
}

int app_ota_is_maintenance(void)
{
    return s_maintenance;
}

int app_ota_cancel_is_requested(void)
{
    return s_cancel_requested;
}

app_ota_state_t app_ota_get_state(void)
{
    return s_state;
}
