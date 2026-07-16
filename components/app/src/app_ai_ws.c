/**
 * @file app_ai_ws.c
 * @brief AI WAI1 WebSocket 传输：持久连接、停等上传、断线续传和 ROP1 下载。
 *
 * socket 只在 biz_ai_ws 任务中读写。语音任务和相机任务提交同步命令，因而不会
 * 出现两个业务同时 recv 同一个连接，网络切换和取消只通过 shutdown 唤醒任务。
 */
#include "app_ai_ws.h"

#include "app_config.h"
#include "osal_heap.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "service_network.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "app_ai_ws";

#define APP_AI_WS_PROTOCOL                 "wai1"
#define APP_AI_WS_HEADER_BYTES             32u
#define APP_AI_WS_FRAME_BYTES              (APP_AI_WS_HEADER_BYTES + APP_AI_WS_MAX_PAYLOAD_BYTES)
#define APP_AI_WS_RX_BUFFER_BYTES          (APP_AI_WS_FRAME_BYTES + 1u)
#define APP_AI_WS_TX_BUFFER_BYTES          (APP_AI_WS_FRAME_BYTES + 8u)
#define APP_AI_WS_TASK_STACK               16384u
#define APP_AI_WS_TASK_PRIORITY            4u
#define APP_AI_WS_QUEUE_DEPTH              2u
#define APP_AI_WS_RECV_SLICE_MS            250u
#define APP_AI_WS_HELLO_TIMEOUT_MS         5000u
#define APP_AI_WS_DEFAULT_HEARTBEAT_MS     10000u
#define APP_AI_WS_WAIT_STEP_MS             20u
#define APP_AI_WS_JSON_BYTES               1536u
#define APP_AI_WS_CAMERA_RESULT_BYTES      1024u
#define APP_AI_WS_OPCODE_CONT              0x00u
#define APP_AI_WS_OPCODE_TEXT              0x01u
#define APP_AI_WS_OPCODE_BINARY            0x02u
#define APP_AI_WS_OPCODE_CLOSE             0x08u
#define APP_AI_WS_OPCODE_PING              0x09u
#define APP_AI_WS_OPCODE_PONG              0x0au
#define APP_AI_WS_WAI_VERSION              1u
#define APP_AI_WS_PKT_VOICE_UPLOAD         1u
#define APP_AI_WS_PKT_CAMERA_UPLOAD        2u
#define APP_AI_WS_PKT_REPLY_OPUS           3u
#define APP_AI_WS_FLAG_FINAL               0x0001u
#define APP_AI_WS_ERR_TIMEOUT              (-901)
#define APP_AI_WS_ERR_DISCONNECTED         (-902)
#define APP_AI_WS_CLIENT_KEY               "d2Fsa2llLWFpLWtleS0wMQ=="

typedef enum {
    APP_AI_WS_CMD_VOICE = 1,
    APP_AI_WS_CMD_CAMERA,
} app_ai_ws_command_kind_t;

typedef struct {
    app_ai_ws_command_kind_t kind;
    const uint8_t *input;
    uint32_t input_len;
    uint8_t *reply;
    uint32_t reply_capacity;
    char request_id[APP_AI_WS_REQUEST_ID_BYTES];
    char sha256[APP_AI_WS_SHA256_TEXT_BYTES];
    char session[APP_AI_WS_SESSION_BYTES];
    app_ai_ws_voice_result_t voice_result;
    uint8_t camera_result[APP_AI_WS_CAMERA_RESULT_BYTES];
    uint32_t camera_result_len;
    volatile int cancel;
    volatile int done;
    int ret;
} app_ai_ws_command_t;

typedef struct {
    char type[32];
    char request_id[APP_AI_WS_REQUEST_ID_BYTES];
    char session[APP_AI_WS_SESSION_BYTES];
    char status[APP_AI_WS_STATUS_BYTES];
    char kind[16];
    char camera_status[APP_AI_WS_STATUS_BYTES];
    char format[24];
    char sha256[APP_AI_WS_SHA256_TEXT_BYTES];
    char error_code[32];
    char answer_text[APP_AI_WS_ANSWER_TEXT_BYTES];
    char error_message[128];
    uint32_t stream_id;
    uint32_t next_offset;
    uint32_t total;
    uint32_t max_payload;
    uint32_t heartbeat_ms;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t bitrate;
    uint32_t sequence;
    uint8_t channels;
    uint8_t accepted;
    uint8_t ready;
    uint8_t retryable;
} app_ai_ws_event_t;

typedef struct {
    uint8_t packet_type;
    uint16_t flags;
    uint32_t stream_id;
    uint32_t sequence;
    uint32_t offset;
    uint32_t total;
    uint16_t payload_len;
    const uint8_t *payload;
} app_ai_ws_wai_frame_t;

static volatile int s_started = 0;
static volatile int s_connected = 0;
static volatile int s_suspended = 0;
static volatile int s_force_reconnect = 0;
static int s_sock = -1;
static osal_task_t s_task = NULL;
static osal_queue_t s_command_queue = NULL;
static osal_mutex_t s_state_mutex = NULL;
static app_ai_ws_command_t *s_active_command = NULL;
static app_ai_ws_command_t *s_claimed_command = NULL;
static volatile int s_operation_claimed = 0;
static char s_active_session[APP_AI_WS_SESSION_BYTES];
static char s_pending_cancel_request[APP_AI_WS_REQUEST_ID_BYTES];
static char s_pending_cancel_session[APP_AI_WS_SESSION_BYTES];
static uint8_t *s_rx_buffer = NULL;
static uint8_t *s_tx_buffer = NULL;
static uint8_t *s_wai_buffer = NULL;
static uint32_t s_server_max_payload = APP_AI_WS_MAX_PAYLOAD_BYTES;
static uint32_t s_heartbeat_ms = APP_AI_WS_DEFAULT_HEARTBEAT_MS;
static uint32_t s_last_rx_ms = 0u;
static uint32_t s_last_ping_ms = 0u;
static uint32_t s_connection_generation = 0u;
static uint32_t s_mask_seed = 0x57414931u;
static uint32_t s_ping_sequence = 0u;

static void app_ai_ws_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1u);
    dst[dst_size - 1u] = '\0';
}

static int app_ai_ws_identifier_is_safe(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    for (const char *p = value; *p != '\0'; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ':')) {
            return 0;
        }
    }
    return 1;
}

static void app_ai_ws_write_u16_le(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void app_ai_ws_write_u32_le(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t app_ai_ws_read_u16_le(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t app_ai_ws_read_u32_le(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static uint32_t app_ai_ws_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static int app_ai_ws_sha256_hex(const uint8_t *data,
                                uint32_t len,
                                char out[APP_AI_WS_SHA256_TEXT_BYTES])
{
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int ret = mbedtls_sha256_starts(&ctx, 0);
    if (ret == 0) {
        ret = mbedtls_sha256_update(&ctx, data, len);
    }
    if (ret == 0) {
        ret = mbedtls_sha256_finish(&ctx, digest);
    }
    mbedtls_sha256_free(&ctx);
    if (ret != 0) {
        return -1;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(digest); i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
    return 0;
}

static int app_ai_ws_deadline_expired(uint32_t deadline_ms)
{
    return (int32_t)(osal_get_tick_ms() - deadline_ms) >= 0;
}

static int app_ai_ws_command_cancelled(const app_ai_ws_command_t *command)
{
    return command != NULL && command->cancel != 0;
}

static int app_ai_ws_set_socket_timeout(int sock, int option, uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };
    return setsockopt(sock, SOL_SOCKET, option, &tv, sizeof(tv)) == 0 ? 0 : -1;
}

static int app_ai_ws_wait_writable(int sock, uint32_t timeout_ms)
{
    fd_set write_fds;
    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    int ret = select(sock + 1, NULL, &write_fds, NULL, &tv);
    return ret > 0 && FD_ISSET(sock, &write_fds) ? 0 : (ret == 0 ? 1 : -1);
}

static int app_ai_ws_send_all(int sock, const uint8_t *data, size_t len, uint32_t budget_ms)
{
    size_t sent = 0u;
    uint32_t start_ms = osal_get_tick_ms();
    while (sent < len) {
        uint32_t used_ms = (uint32_t)(osal_get_tick_ms() - start_ms);
        if (used_ms >= budget_ms || s_force_reconnect != 0 || service_network_is_ready() != 1) {
            return -2;
        }
        uint32_t wait_ms = budget_ms - used_ms;
        if (wait_ms > APP_AI_WS_RECV_SLICE_MS) {
            wait_ms = APP_AI_WS_RECV_SLICE_MS;
        }
        int wait_ret = app_ai_ws_wait_writable(sock, wait_ms);
        if (wait_ret > 0) {
            continue;
        }
        if (wait_ret < 0) {
            return -3;
        }

        int flags = 0;
#ifdef MSG_DONTWAIT
        flags = MSG_DONTWAIT;
#endif
        int ret = send(sock, &data[sent], len - sent, flags);
        if (ret > 0) {
            sent += (size_t)ret;
        } else if (ret < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            osal_delay_ms(1u);
        } else {
            return -4;
        }
    }
    return 0;
}

static int app_ai_ws_recv_exact(int sock, uint8_t *data, size_t len, int allow_idle)
{
    size_t received = 0u;
    uint32_t start_ms = osal_get_tick_ms();
    while (received < len) {
        int ret = recv(sock, &data[received], len - received, 0);
        if (ret > 0) {
            received += (size_t)ret;
            continue;
        }
        if (ret == 0) {
            return APP_AI_WS_ERR_DISCONNECTED;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (received == 0u && allow_idle != 0) {
                return 1;
            }
            if ((uint32_t)(osal_get_tick_ms() - start_ms) >= APP_AI_WS_IO_TIMEOUT_MS) {
                return APP_AI_WS_ERR_TIMEOUT;
            }
            if (s_force_reconnect != 0 || service_network_is_ready() != 1) {
                return APP_AI_WS_ERR_DISCONNECTED;
            }
            continue;
        }
        return -3;
    }
    return 0;
}

static int app_ai_ws_drain(int sock, uint64_t len)
{
    uint8_t scratch[64];
    while (len > 0u) {
        size_t take = len > sizeof(scratch) ? sizeof(scratch) : (size_t)len;
        if (app_ai_ws_recv_exact(sock, scratch, take, 0) != 0) {
            return -1;
        }
        len -= take;
    }
    return 0;
}

static int app_ai_ws_send_frame(uint8_t opcode, const uint8_t *payload, uint16_t payload_len)
{
    if (s_sock < 0 || s_tx_buffer == NULL ||
        payload_len > APP_AI_WS_FRAME_BYTES ||
        (payload_len > 0u && payload == NULL)) {
        return -1;
    }

    uint16_t pos = 0u;
    uint8_t mask[4];
    s_mask_seed = s_mask_seed * 1664525u + 1013904223u + osal_get_tick_ms();
    app_ai_ws_write_u32_le(mask, s_mask_seed);
    s_tx_buffer[pos++] = (uint8_t)(0x80u | (opcode & 0x0fu));
    if (payload_len <= 125u) {
        s_tx_buffer[pos++] = (uint8_t)(0x80u | payload_len);
    } else {
        s_tx_buffer[pos++] = 0x80u | 126u;
        s_tx_buffer[pos++] = (uint8_t)((payload_len >> 8) & 0xffu);
        s_tx_buffer[pos++] = (uint8_t)(payload_len & 0xffu);
    }
    memcpy(&s_tx_buffer[pos], mask, sizeof(mask));
    pos = (uint16_t)(pos + sizeof(mask));
    for (uint16_t i = 0u; i < payload_len; i++) {
        s_tx_buffer[pos + i] = payload[i] ^ mask[i % 4u];
    }
    return app_ai_ws_send_all(s_sock,
                              s_tx_buffer,
                              (size_t)pos + payload_len,
                              APP_AI_WS_IO_TIMEOUT_MS);
}

static int app_ai_ws_send_json(const char *json)
{
    size_t len = json != NULL ? strlen(json) : 0u;
    if (len == 0u || len > APP_AI_WS_FRAME_BYTES) {
        return -1;
    }
    return app_ai_ws_send_frame(APP_AI_WS_OPCODE_TEXT, (const uint8_t *)json, (uint16_t)len);
}

static int app_ai_ws_recv_frame(uint8_t *opcode, uint16_t *payload_len)
{
    uint8_t header[2];
    int ret = app_ai_ws_recv_exact(s_sock, header, sizeof(header), 1);
    if (ret != 0) {
        return ret;
    }

    uint8_t fin = header[0] & 0x80u;
    uint8_t frame_opcode = header[0] & 0x0fu;
    uint8_t masked = header[1] & 0x80u;
    uint64_t len = header[1] & 0x7fu;
    if (len == 126u) {
        uint8_t ext[2];
        if (app_ai_ws_recv_exact(s_sock, ext, sizeof(ext), 0) != 0) {
            return -2;
        }
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127u) {
        uint8_t ext[8];
        if (app_ai_ws_recv_exact(s_sock, ext, sizeof(ext), 0) != 0) {
            return -3;
        }
        len = 0u;
        for (uint8_t i = 0u; i < sizeof(ext); i++) {
            len = (len << 8) | ext[i];
        }
    }

    uint8_t mask[4] = {0};
    if (masked != 0u && app_ai_ws_recv_exact(s_sock, mask, sizeof(mask), 0) != 0) {
        return -4;
    }
    if (len > APP_AI_WS_FRAME_BYTES) {
        (void)app_ai_ws_drain(s_sock, len);
        return -5;
    }

    uint16_t frame_len = (uint16_t)len;
    if (frame_len > 0u && app_ai_ws_recv_exact(s_sock, s_rx_buffer, frame_len, 0) != 0) {
        return -6;
    }
    if (masked != 0u) {
        for (uint16_t i = 0u; i < frame_len; i++) {
            s_rx_buffer[i] ^= mask[i % 4u];
        }
    }
    if (fin == 0u || frame_opcode == APP_AI_WS_OPCODE_CONT) {
        return -7;
    }
    if (frame_opcode == APP_AI_WS_OPCODE_CLOSE) {
        return APP_AI_WS_ERR_DISCONNECTED;
    }
    if (frame_opcode == APP_AI_WS_OPCODE_PING) {
        (void)app_ai_ws_send_frame(APP_AI_WS_OPCODE_PONG, s_rx_buffer, frame_len);
        return 2;
    }
    if (frame_opcode == APP_AI_WS_OPCODE_PONG) {
        return 2;
    }
    if (frame_opcode != APP_AI_WS_OPCODE_TEXT && frame_opcode != APP_AI_WS_OPCODE_BINARY) {
        return 2;
    }

    *opcode = frame_opcode;
    *payload_len = frame_len;
    return 0;
}

static void app_ai_ws_json_copy_string(cJSON *root,
                                       const char *key,
                                       char *out,
                                       size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        app_ai_ws_copy_text(out, out_size, item->valuestring);
    }
}

static uint32_t app_ai_ws_json_u32(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 4294967295.0 ?
           (uint32_t)item->valuedouble : 0u;
}

static int app_ai_ws_parse_event(const uint8_t *json,
                                 uint16_t len,
                                 app_ai_ws_event_t *event)
{
    if (json == NULL || len == 0u || event == NULL) {
        return -1;
    }
    memset(event, 0, sizeof(*event));
    cJSON *root = cJSON_ParseWithLength((const char *)json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -2;
    }

    app_ai_ws_json_copy_string(root, "type", event->type, sizeof(event->type));
    app_ai_ws_json_copy_string(root, "request_id", event->request_id, sizeof(event->request_id));
    app_ai_ws_json_copy_string(root, "session", event->session, sizeof(event->session));
    app_ai_ws_json_copy_string(root, "status", event->status, sizeof(event->status));
    app_ai_ws_json_copy_string(root, "kind", event->kind, sizeof(event->kind));
    app_ai_ws_json_copy_string(root, "camera_status", event->camera_status, sizeof(event->camera_status));
    app_ai_ws_json_copy_string(root, "format", event->format, sizeof(event->format));
    app_ai_ws_json_copy_string(root, "sha256", event->sha256, sizeof(event->sha256));
    app_ai_ws_json_copy_string(root, "code", event->error_code, sizeof(event->error_code));
    app_ai_ws_json_copy_string(root, "answer_text", event->answer_text, sizeof(event->answer_text));
    app_ai_ws_json_copy_string(root, "error_message", event->error_message, sizeof(event->error_message));
    if (event->error_message[0] == '\0') {
        app_ai_ws_json_copy_string(root, "message", event->error_message, sizeof(event->error_message));
    }
    event->stream_id = app_ai_ws_json_u32(root, "stream_id");
    event->next_offset = app_ai_ws_json_u32(root, "next_offset");
    event->total = app_ai_ws_json_u32(root, "total");
    event->max_payload = app_ai_ws_json_u32(root, "max_payload");
    event->heartbeat_ms = app_ai_ws_json_u32(root, "heartbeat_ms");
    event->duration_ms = app_ai_ws_json_u32(root, "duration_ms");
    event->sample_rate = app_ai_ws_json_u32(root, "sample_rate");
    event->frame_ms = app_ai_ws_json_u32(root, "frame_ms");
    event->bitrate = app_ai_ws_json_u32(root, "bitrate");
    event->sequence = app_ai_ws_json_u32(root, "seq");
    event->channels = (uint8_t)app_ai_ws_json_u32(root, "channels");
    event->accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "accepted")) ? 1u : 0u;
    event->ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ready")) ? 1u : 0u;
    event->retryable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "retryable")) ? 1u : 0u;
    cJSON_Delete(root);
    return event->type[0] != '\0' ? 0 : -3;
}

static int app_ai_ws_wait_application_frame(uint32_t timeout_ms,
                                            uint8_t *opcode,
                                            uint16_t *payload_len)
{
    uint32_t start_ms = osal_get_tick_ms();
    while ((uint32_t)(osal_get_tick_ms() - start_ms) < timeout_ms) {
        int ret = app_ai_ws_recv_frame(opcode, payload_len);
        if (ret == 0) {
            uint32_t now = osal_get_tick_ms();
            s_last_rx_ms = now;
            if (*opcode == APP_AI_WS_OPCODE_TEXT) {
                s_rx_buffer[*payload_len] = '\0';
                app_ai_ws_event_t event;
                if (app_ai_ws_parse_event(s_rx_buffer, *payload_len, &event) == 0) {
                    if (strcmp(event.type, "ping") == 0) {
                        char pong[64];
                        snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"seq\":%u}",
                                 (unsigned int)event.sequence);
                        (void)app_ai_ws_send_json(pong);
                        continue;
                    }
                    if (strcmp(event.type, "pong") == 0) {
                        continue;
                    }
                }
            }
            return 0;
        }
        if (ret < 0) {
            return ret;
        }

        uint32_t now = osal_get_tick_ms();
        if ((uint32_t)(now - s_last_ping_ms) >= s_heartbeat_ms) {
            char ping[64];
            s_ping_sequence++;
            snprintf(ping, sizeof(ping), "{\"type\":\"ping\",\"seq\":%u}",
                     (unsigned int)s_ping_sequence);
            if (app_ai_ws_send_json(ping) != 0) {
                return APP_AI_WS_ERR_DISCONNECTED;
            }
            s_last_ping_ms = now;
        }
        if ((uint32_t)(now - s_last_rx_ms) >= APP_AI_WS_IDLE_TIMEOUT_MS) {
            return APP_AI_WS_ERR_TIMEOUT;
        }
        if (s_force_reconnect != 0 || service_network_is_ready() != 1 || s_suspended != 0) {
            return APP_AI_WS_ERR_DISCONNECTED;
        }
    }
    return 1;
}

static void app_ai_ws_worker_close(int ret)
{
    int sock = s_sock;
    s_connected = 0;
    if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        if (s_sock == sock) {
            s_sock = -1;
        }
        osal_mutex_unlock(s_state_mutex);
    } else {
        s_sock = -1;
    }
    if (sock >= 0) {
        close(sock);
    }
    if (ret != 0 && service_network_is_ready() == 1 && s_suspended == 0) {
        APP_LOGW(TAG, "ai_ws event=disconnected device=%s ret=%d", APP_DEVICE_ID, ret);
    }
}

static void app_ai_ws_interrupt_socket(void)
{
    if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        s_force_reconnect = 1;
        if (s_sock >= 0) {
            (void)shutdown(s_sock, SHUT_RDWR);
        }
        osal_mutex_unlock(s_state_mutex);
    } else {
        s_force_reconnect = 1;
    }
    if (s_task != NULL) {
        (void)osal_task_notify_give(s_task);
    }
}

static int app_ai_ws_handshake(int sock)
{
    char request[448];
    char response[768];
    int len = snprintf(request,
                       sizeof(request),
                       "GET %s?device=%s&protocol=%s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Sec-WebSocket-Key: %s\r\n\r\n",
                       APP_BUSINESS_AI_WS_ROUTE,
                       APP_DEVICE_ID,
                       APP_AI_WS_PROTOCOL,
                       APP_BUSINESS_SERVER_HOST,
                       APP_BUSINESS_AI_WS_PORT,
                       APP_AI_WS_CLIENT_KEY);
    if (len <= 0 || (size_t)len >= sizeof(request) ||
        app_ai_ws_send_all(sock, (const uint8_t *)request, (size_t)len,
                           APP_AI_WS_CONNECT_TIMEOUT_MS) != 0) {
        return -1;
    }

    size_t used = 0u;
    while (used + 1u < sizeof(response)) {
        uint8_t byte = 0u;
        int ret = app_ai_ws_recv_exact(sock, &byte, 1u, 0);
        if (ret != 0) {
            return -2;
        }
        response[used++] = (char)byte;
        response[used] = '\0';
        if (used >= 4u && memcmp(&response[used - 4u], "\r\n\r\n", 4u) == 0) {
            break;
        }
    }
    if (strstr(response, " 101 ") == NULL && strstr(response, " 101\r\n") == NULL) {
        return -3;
    }
    return 0;
}

static int app_ai_ws_connect_once(void)
{
    if (service_network_is_ready() != 1 || s_suspended != 0) {
        return -1;
    }
    char port_text[8];
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addresses = NULL;
    snprintf(port_text, sizeof(port_text), "%d", APP_BUSINESS_AI_WS_PORT);
    if (getaddrinfo(APP_BUSINESS_SERVER_HOST, port_text, &hints, &addresses) != 0 ||
        addresses == NULL) {
        return -2;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        freeaddrinfo(addresses);
        return -3;
    }
    (void)app_ai_ws_set_socket_timeout(sock, SO_RCVTIMEO, APP_AI_WS_CONNECT_TIMEOUT_MS);
    (void)app_ai_ws_set_socket_timeout(sock, SO_SNDTIMEO, APP_AI_WS_CONNECT_TIMEOUT_MS);
    if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        s_sock = sock;
        osal_mutex_unlock(s_state_mutex);
    } else {
        s_sock = sock;
    }
    int connect_ret = connect(sock, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connect_ret != 0) {
        app_ai_ws_worker_close(-4);
        return -4;
    }
    if (app_ai_ws_handshake(sock) != 0) {
        app_ai_ws_worker_close(-5);
        return -5;
    }

    (void)app_ai_ws_set_socket_timeout(sock, SO_RCVTIMEO, APP_AI_WS_RECV_SLICE_MS);
    (void)app_ai_ws_set_socket_timeout(sock, SO_SNDTIMEO, APP_AI_WS_IO_TIMEOUT_MS);
    s_force_reconnect = 0;
    s_last_rx_ms = osal_get_tick_ms();
    s_last_ping_ms = s_last_rx_ms;

    uint8_t opcode = 0u;
    uint16_t payload_len = 0u;
    int ret = app_ai_ws_wait_application_frame(APP_AI_WS_HELLO_TIMEOUT_MS,
                                                &opcode,
                                                &payload_len);
    if (ret != 0 || opcode != APP_AI_WS_OPCODE_TEXT) {
        app_ai_ws_worker_close(ret != 0 ? ret : -6);
        return -6;
    }
    app_ai_ws_event_t hello;
    if (app_ai_ws_parse_event(s_rx_buffer, payload_len, &hello) != 0 ||
        strcmp(hello.type, "hello") != 0) {
        app_ai_ws_worker_close(-7);
        return -7;
    }
    cJSON *hello_root = cJSON_ParseWithLength((const char *)s_rx_buffer, payload_len);
    cJSON *protocol = cJSON_IsObject(hello_root) ?
                      cJSON_GetObjectItemCaseSensitive(hello_root, "protocol") : NULL;
    int protocol_ok = cJSON_IsString(protocol) && protocol->valuestring != NULL &&
                      strcmp(protocol->valuestring, APP_AI_WS_PROTOCOL) == 0;
    cJSON_Delete(hello_root);
    if (!protocol_ok) {
        app_ai_ws_worker_close(-8);
        return -8;
    }

    s_server_max_payload = hello.max_payload > 0u &&
                           hello.max_payload <= APP_AI_WS_MAX_PAYLOAD_BYTES ?
                           hello.max_payload : APP_AI_WS_MAX_PAYLOAD_BYTES;
    s_heartbeat_ms = hello.heartbeat_ms >= 3000u &&
                     hello.heartbeat_ms < APP_AI_WS_IDLE_TIMEOUT_MS ?
                     hello.heartbeat_ms : APP_AI_WS_DEFAULT_HEARTBEAT_MS;
    s_connected = 1;
    s_connection_generation++;
    APP_LOGI(TAG,
             "ai_ws event=connected device=%s host=%s port=%d route=%s max_payload=%u heartbeat_ms=%u",
             APP_DEVICE_ID,
             APP_BUSINESS_SERVER_HOST,
             APP_BUSINESS_AI_WS_PORT,
             APP_BUSINESS_AI_WS_ROUTE,
             (unsigned int)s_server_max_payload,
             (unsigned int)s_heartbeat_ms);
    return 0;
}

static int app_ai_ws_ensure_connected(app_ai_ws_command_t *command, uint32_t deadline_ms)
{
    uint32_t delay_ms = APP_AI_WS_RECONNECT_MS;
    while (s_connected == 0) {
        if (app_ai_ws_command_cancelled(command)) {
            return APP_AI_WS_ERR_CANCELLED;
        }
        if (app_ai_ws_deadline_expired(deadline_ms)) {
            return APP_AI_WS_ERR_TIMEOUT;
        }
        if (s_suspended != 0) {
            return APP_AI_WS_ERR_CANCELLED;
        }
        if (service_network_is_ready() != 1) {
            (void)osal_task_notify_take(500u);
            continue;
        }

        s_force_reconnect = 0;
        int ret = app_ai_ws_connect_once();
        if (ret == 0) {
            return 0;
        }
        APP_LOGW(TAG,
                 "ai_ws event=connect_fail device=%s ret=%d retry_ms=%u",
                 APP_DEVICE_ID,
                 ret,
                 (unsigned int)delay_ms);
        (void)osal_task_notify_take(delay_ms);
        delay_ms = delay_ms < APP_AI_WS_RECONNECT_MAX_MS / 2u ?
                   delay_ms * 2u : APP_AI_WS_RECONNECT_MAX_MS;
    }
    return 0;
}

static void app_ai_ws_set_active_identity(const app_ai_ws_command_t *command)
{
    if (s_state_mutex == NULL || osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) != 0) {
        return;
    }
    app_ai_ws_copy_text(s_active_session,
                        sizeof(s_active_session),
                        command != NULL ? command->session : NULL);
    osal_mutex_unlock(s_state_mutex);
}

static int app_ai_ws_build_wai_frame(uint8_t packet_type,
                                     uint16_t flags,
                                     uint32_t stream_id,
                                     uint32_t sequence,
                                     uint32_t offset,
                                     uint32_t total,
                                     const uint8_t *payload,
                                     uint16_t payload_len,
                                     uint16_t *frame_len)
{
    if (s_wai_buffer == NULL || payload == NULL || frame_len == NULL ||
        payload_len == 0u || payload_len > APP_AI_WS_MAX_PAYLOAD_BYTES) {
        return -1;
    }
    memcpy(&s_wai_buffer[0], "WAI1", 4u);
    s_wai_buffer[4] = APP_AI_WS_WAI_VERSION;
    s_wai_buffer[5] = packet_type;
    app_ai_ws_write_u16_le(&s_wai_buffer[6], flags);
    app_ai_ws_write_u32_le(&s_wai_buffer[8], stream_id);
    app_ai_ws_write_u32_le(&s_wai_buffer[12], sequence);
    app_ai_ws_write_u32_le(&s_wai_buffer[16], offset);
    app_ai_ws_write_u32_le(&s_wai_buffer[20], total);
    app_ai_ws_write_u16_le(&s_wai_buffer[24], payload_len);
    app_ai_ws_write_u16_le(&s_wai_buffer[26], 0u);
    app_ai_ws_write_u32_le(&s_wai_buffer[28], app_ai_ws_crc32(payload, payload_len));
    memcpy(&s_wai_buffer[APP_AI_WS_HEADER_BYTES], payload, payload_len);
    *frame_len = (uint16_t)(APP_AI_WS_HEADER_BYTES + payload_len);
    return 0;
}

static int app_ai_ws_parse_wai_frame(const uint8_t *frame,
                                     uint16_t frame_len,
                                     app_ai_ws_wai_frame_t *wai)
{
    if (frame == NULL || wai == NULL || frame_len < APP_AI_WS_HEADER_BYTES ||
        memcmp(frame, "WAI1", 4u) != 0 || frame[4] != APP_AI_WS_WAI_VERSION) {
        return -1;
    }
    memset(wai, 0, sizeof(*wai));
    wai->packet_type = frame[5];
    wai->flags = app_ai_ws_read_u16_le(&frame[6]);
    wai->stream_id = app_ai_ws_read_u32_le(&frame[8]);
    wai->sequence = app_ai_ws_read_u32_le(&frame[12]);
    wai->offset = app_ai_ws_read_u32_le(&frame[16]);
    wai->total = app_ai_ws_read_u32_le(&frame[20]);
    wai->payload_len = app_ai_ws_read_u16_le(&frame[24]);
    wai->payload = &frame[APP_AI_WS_HEADER_BYTES];
    if ((uint32_t)frame_len != APP_AI_WS_HEADER_BYTES + wai->payload_len ||
        wai->payload_len == 0u ||
        app_ai_ws_read_u32_le(&frame[28]) != app_ai_ws_crc32(wai->payload, wai->payload_len)) {
        return -2;
    }
    return 0;
}

static int app_ai_ws_send_start(app_ai_ws_command_t *command)
{
    char json[APP_AI_WS_JSON_BYTES];
    const char *type = command->kind == APP_AI_WS_CMD_VOICE ? "voice_start" : "camera_start";
    int len;
    if (command->kind == APP_AI_WS_CMD_VOICE) {
        len = snprintf(json,
                       sizeof(json),
                       "{\"type\":\"%s\",\"request_id\":\"%s\",\"language\":\"zh\","
                       "\"input_format\":\"opus_packets_v1\",\"total\":%u,\"sha256\":\"%s\"}",
                       type,
                       command->request_id,
                       (unsigned int)command->input_len,
                       command->sha256);
    } else {
        len = snprintf(json,
                       sizeof(json),
                       "{\"type\":\"%s\",\"request_id\":\"%s\",\"content_type\":\"image/jpeg\","
                       "\"total\":%u,\"sha256\":\"%s\"}",
                       type,
                       command->request_id,
                       (unsigned int)command->input_len,
                       command->sha256);
    }
    return len > 0 && (size_t)len < sizeof(json) ? app_ai_ws_send_json(json) : -1;
}

static int app_ai_ws_wait_started(app_ai_ws_command_t *command,
                                  uint32_t *stream_id,
                                  uint32_t *next_offset)
{
    const char *expected = command->kind == APP_AI_WS_CMD_VOICE ?
                           "voice_started" : "camera_started";
    uint32_t start_ms = osal_get_tick_ms();
    while ((uint32_t)(osal_get_tick_ms() - start_ms) < APP_AI_WS_IO_TIMEOUT_MS) {
        uint8_t opcode = 0u;
        uint16_t len = 0u;
        int ret = app_ai_ws_wait_application_frame(APP_AI_WS_IO_TIMEOUT_MS, &opcode, &len);
        if (ret != 0) {
            return ret == 1 ? APP_AI_WS_ERR_TIMEOUT : ret;
        }
        if (opcode != APP_AI_WS_OPCODE_TEXT) {
            continue;
        }
        app_ai_ws_event_t event;
        if (app_ai_ws_parse_event(s_rx_buffer, len, &event) != 0) {
            continue;
        }
        if (strcmp(event.type, "error") == 0) {
            APP_LOGW(TAG, "ai_ws event=server_error stage=start message=%s", event.error_message);
            return -20;
        }
        if (strcmp(event.type, expected) != 0) {
            continue;
        }
        if (!app_ai_ws_identifier_is_safe(event.session) || event.next_offset > command->input_len) {
            return -21;
        }
        app_ai_ws_copy_text(command->session, sizeof(command->session), event.session);
        *stream_id = event.stream_id;
        *next_offset = event.next_offset;
        if (event.max_payload > 0u && event.max_payload < s_server_max_payload) {
            s_server_max_payload = event.max_payload;
        }
        app_ai_ws_set_active_identity(command);
        return 0;
    }
    return APP_AI_WS_ERR_TIMEOUT;
}

static int app_ai_ws_upload(app_ai_ws_command_t *command, uint32_t deadline_ms)
{
    uint32_t offset = 0u;
    uint32_t sequence = 0u;
    uint32_t last_log_offset = 0u;
    while (offset < command->input_len) {
        if (app_ai_ws_command_cancelled(command)) {
            return APP_AI_WS_ERR_CANCELLED;
        }
        int ret = app_ai_ws_ensure_connected(command, deadline_ms);
        if (ret != 0) {
            return ret;
        }
        if (app_ai_ws_send_start(command) != 0) {
            app_ai_ws_worker_close(-22);
            continue;
        }

        uint32_t stream_id = 0u;
        uint32_t server_offset = 0u;
        ret = app_ai_ws_wait_started(command, &stream_id, &server_offset);
        if (ret != 0) {
            if (ret == -20 || ret == -21) {
                return ret;
            }
            app_ai_ws_worker_close(ret);
            continue;
        }
        offset = server_offset;
        uint8_t duplicate_ack = 0u;
        while (offset < command->input_len && s_connected != 0) {
            if (app_ai_ws_command_cancelled(command)) {
                return APP_AI_WS_ERR_CANCELLED;
            }
            uint32_t chunk_len = command->input_len - offset;
            if (chunk_len > s_server_max_payload) {
                chunk_len = s_server_max_payload;
            }
            uint16_t wai_len = 0u;
            uint16_t flags = offset + chunk_len == command->input_len ? APP_AI_WS_FLAG_FINAL : 0u;
            uint8_t packet_type = command->kind == APP_AI_WS_CMD_VOICE ?
                                  APP_AI_WS_PKT_VOICE_UPLOAD : APP_AI_WS_PKT_CAMERA_UPLOAD;
            if (app_ai_ws_build_wai_frame(packet_type,
                                          flags,
                                          stream_id,
                                          sequence++,
                                          offset,
                                          command->input_len,
                                          &command->input[offset],
                                          (uint16_t)chunk_len,
                                          &wai_len) != 0 ||
                app_ai_ws_send_frame(APP_AI_WS_OPCODE_BINARY, s_wai_buffer, wai_len) != 0) {
                app_ai_ws_worker_close(-23);
                break;
            }

            uint8_t opcode = 0u;
            uint16_t response_len = 0u;
            ret = app_ai_ws_wait_application_frame(APP_AI_WS_IO_TIMEOUT_MS, &opcode, &response_len);
            if (ret != 0) {
                app_ai_ws_worker_close(ret == 1 ? APP_AI_WS_ERR_TIMEOUT : ret);
                break;
            }
            if (opcode != APP_AI_WS_OPCODE_TEXT) {
                continue;
            }
            app_ai_ws_event_t ack;
            if (app_ai_ws_parse_event(s_rx_buffer, response_len, &ack) != 0) {
                continue;
            }
            if (strcmp(ack.type, "error") == 0) {
                if (ack.retryable != 0u && ack.next_offset <= command->input_len) {
                    APP_LOGW(TAG,
                             "ai_ws_upload event=resync code=%s local=%u server=%u",
                             ack.error_code,
                             (unsigned int)offset,
                             (unsigned int)ack.next_offset);
                    offset = ack.next_offset;
                    duplicate_ack = 0u;
                    continue;
                }
                APP_LOGW(TAG, "ai_ws event=server_error stage=upload message=%s", ack.error_message);
                return -24;
            }
            if (strcmp(ack.type, "upload_ack") != 0 ||
                ack.stream_id != stream_id ||
                ack.next_offset > command->input_len) {
                continue;
            }
            if (ack.next_offset == offset) {
                duplicate_ack++;
                if (duplicate_ack >= 3u) {
                    app_ai_ws_worker_close(-25);
                    break;
                }
                continue;
            }
            if (ack.next_offset < offset || ack.next_offset > offset + chunk_len) {
                app_ai_ws_worker_close(-26);
                break;
            }
            duplicate_ack = 0u;
            offset = ack.next_offset;
            if (offset == command->input_len || offset - last_log_offset >= 65536u) {
                APP_LOGI(TAG,
                         "ai_ws_upload kind=%s request_id=%s offset=%u total=%u connected=1",
                         command->kind == APP_AI_WS_CMD_VOICE ? "voice" : "camera",
                         command->request_id,
                         (unsigned int)offset,
                         (unsigned int)command->input_len);
                last_log_offset = offset;
            }
        }
    }
    return 0;
}

static int app_ai_ws_send_finish(const app_ai_ws_command_t *command)
{
    char json[APP_AI_WS_JSON_BYTES];
    const char *type = command->kind == APP_AI_WS_CMD_VOICE ? "voice_finish" : "camera_finish";
    int len = snprintf(json,
                       sizeof(json),
                       "{\"type\":\"%s\",\"request_id\":\"%s\",\"session\":\"%s\","
                       "\"sha256\":\"%s\"}",
                       type,
                       command->request_id,
                       command->session,
                       command->sha256);
    return len > 0 && (size_t)len < sizeof(json) ? app_ai_ws_send_json(json) : -1;
}

static int app_ai_ws_send_resume(const app_ai_ws_command_t *command)
{
    char json[APP_AI_WS_JSON_BYTES];
    int len = snprintf(json,
                       sizeof(json),
                       "{\"type\":\"session_resume\",\"request_id\":\"%s\",\"session\":\"%s\"}",
                       command->request_id,
                       command->session);
    return len > 0 && (size_t)len < sizeof(json) ? app_ai_ws_send_json(json) : -1;
}

static int app_ai_ws_verify_reply(const uint8_t *reply, uint32_t reply_len, const char *expected_sha)
{
    char actual[APP_AI_WS_SHA256_TEXT_BYTES];
    return expected_sha != NULL && strlen(expected_sha) == 64u &&
           app_ai_ws_sha256_hex(reply, reply_len, actual) == 0 &&
           strcmp(actual, expected_sha) == 0 ? 0 : -1;
}

static int app_ai_ws_download_reply(app_ai_ws_command_t *command,
                                    const app_ai_ws_event_t *ready,
                                    uint32_t deadline_ms)
{
    if (ready->total == 0u || ready->total > command->reply_capacity ||
        ready->total > APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES ||
        strcmp(ready->format, "rop1") != 0 || strlen(ready->sha256) != 64u) {
        return -40;
    }

    uint32_t offset = 0u;
    uint32_t requested_generation = 0u;
    while (offset < ready->total) {
        if (app_ai_ws_command_cancelled(command)) {
            return APP_AI_WS_ERR_CANCELLED;
        }
        int ret = app_ai_ws_ensure_connected(command, deadline_ms);
        if (ret != 0) {
            return ret;
        }
        if (requested_generation != s_connection_generation) {
            char get_json[256];
            int len = snprintf(get_json,
                               sizeof(get_json),
                               "{\"type\":\"reply_get\",\"session\":\"%s\",\"offset\":%u}",
                               command->session,
                               (unsigned int)offset);
            if (len <= 0 || (size_t)len >= sizeof(get_json) || app_ai_ws_send_json(get_json) != 0) {
                app_ai_ws_worker_close(-41);
                continue;
            }
            requested_generation = s_connection_generation;
        }

        uint8_t opcode = 0u;
        uint16_t frame_len = 0u;
        ret = app_ai_ws_wait_application_frame(APP_AI_WS_IO_TIMEOUT_MS, &opcode, &frame_len);
        if (ret == 1) {
            requested_generation = 0u;
            continue;
        }
        if (ret != 0) {
            app_ai_ws_worker_close(ret);
            requested_generation = 0u;
            continue;
        }
        if (opcode == APP_AI_WS_OPCODE_TEXT) {
            app_ai_ws_event_t event;
            if (app_ai_ws_parse_event(s_rx_buffer, frame_len, &event) == 0 &&
                strcmp(event.type, "error") == 0) {
                APP_LOGW(TAG, "ai_ws event=server_error stage=reply_get message=%s", event.error_message);
                return -42;
            }
            continue;
        }

        app_ai_ws_wai_frame_t wai;
        if (app_ai_ws_parse_wai_frame(s_rx_buffer, frame_len, &wai) != 0 ||
            wai.packet_type != APP_AI_WS_PKT_REPLY_OPUS ||
            wai.total != ready->total ||
            (ready->stream_id != 0u && wai.stream_id != ready->stream_id)) {
            app_ai_ws_worker_close(-43);
            requested_generation = 0u;
            continue;
        }
        if (wai.offset < offset) {
            char ack_json[256];
            snprintf(ack_json,
                     sizeof(ack_json),
                     "{\"type\":\"reply_ack\",\"session\":\"%s\",\"stream_id\":%u,\"next_offset\":%u}",
                     command->session,
                     (unsigned int)wai.stream_id,
                     (unsigned int)offset);
            (void)app_ai_ws_send_json(ack_json);
            continue;
        }
        if (wai.offset != offset || wai.payload_len > ready->total - offset) {
            app_ai_ws_worker_close(-44);
            requested_generation = 0u;
            continue;
        }

        memcpy(&command->reply[offset], wai.payload, wai.payload_len);
        offset += wai.payload_len;
        char ack_json[256];
        int ack_len = snprintf(ack_json,
                               sizeof(ack_json),
                               "{\"type\":\"reply_ack\",\"session\":\"%s\",\"stream_id\":%u,\"next_offset\":%u}",
                               command->session,
                               (unsigned int)wai.stream_id,
                               (unsigned int)offset);
        if (ack_len <= 0 || (size_t)ack_len >= sizeof(ack_json) || app_ai_ws_send_json(ack_json) != 0) {
            app_ai_ws_worker_close(-45);
            requested_generation = 0u;
            continue;
        }
    }

    if (app_ai_ws_verify_reply(command->reply, ready->total, ready->sha256) != 0) {
        return -46;
    }

    int server_completed = 0;
    uint8_t complete_opcode = 0u;
    uint16_t complete_len = 0u;
    int complete_ret = app_ai_ws_wait_application_frame(1500u,
                                                         &complete_opcode,
                                                         &complete_len);
    if (complete_ret == 0 && complete_opcode == APP_AI_WS_OPCODE_TEXT) {
        app_ai_ws_event_t complete_event;
        if (app_ai_ws_parse_event(s_rx_buffer, complete_len, &complete_event) == 0 &&
            strcmp(complete_event.type, "reply_complete") == 0 &&
            strcmp(complete_event.session, command->session) == 0) {
            if (complete_event.total != ready->total ||
                strcmp(complete_event.sha256, ready->sha256) != 0) {
                return -47;
            }
            server_completed = 1;
        }
    }
    if (!server_completed) {
        /* 协议约定由设备确认；兼容首版后端在最后一个 ACK 后主动完成。 */
        char complete[288];
        int len = snprintf(complete,
                           sizeof(complete),
                           "{\"type\":\"reply_complete\",\"session\":\"%s\",\"total\":%u,\"sha256\":\"%s\"}",
                           command->session,
                           (unsigned int)ready->total,
                           ready->sha256);
        if (len <= 0 || (size_t)len >= sizeof(complete) || app_ai_ws_send_json(complete) != 0) {
            APP_LOGW(TAG,
                     "ai_ws_reply event=complete_notify_fail session=%s verified=1",
                     command->session);
        }
    }
    command->voice_result.reply_bytes = ready->total;
    command->voice_result.duration_ms = ready->duration_ms;
    command->voice_result.sample_rate = ready->sample_rate;
    command->voice_result.frame_ms = ready->frame_ms;
    command->voice_result.bitrate = ready->bitrate;
    command->voice_result.channels = ready->channels;
    APP_LOGI(TAG,
             "ai_ws_reply event=verified session=%s bytes=%u duration_ms=%u format=rop1",
             command->session,
             (unsigned int)ready->total,
             (unsigned int)ready->duration_ms);
    return 0;
}

static int app_ai_ws_camera_result_is_terminal(const app_ai_ws_event_t *event)
{
    return (strcmp(event->kind, "camera") == 0 && strcmp(event->status, "text_ready") == 0) ||
           strcmp(event->camera_status, "ready") == 0 ||
           strcmp(event->status, "ready") == 0 ||
           strcmp(event->status, "image_ready") == 0 ||
           strcmp(event->status, "completed") == 0 ||
           strcmp(event->status, "success") == 0 ||
           strcmp(event->status, "failed") == 0 ||
           strcmp(event->status, "error") == 0 ||
           strcmp(event->status, "cancelled") == 0 ||
           strcmp(event->status, "canceled") == 0 ||
           event->accepted != 0u || event->ready != 0u || event->answer_text[0] != '\0';
}

static int app_ai_ws_wait_result(app_ai_ws_command_t *command, uint32_t deadline_ms)
{
    uint32_t announced_generation = 0u;
    while (!app_ai_ws_deadline_expired(deadline_ms)) {
        if (app_ai_ws_command_cancelled(command)) {
            return APP_AI_WS_ERR_CANCELLED;
        }
        int ret = app_ai_ws_ensure_connected(command, deadline_ms);
        if (ret != 0) {
            return ret;
        }
        if (announced_generation != s_connection_generation) {
            if (announced_generation != 0u && app_ai_ws_send_resume(command) != 0) {
                app_ai_ws_worker_close(-50);
                continue;
            }
            if (app_ai_ws_send_finish(command) != 0) {
                app_ai_ws_worker_close(-51);
                continue;
            }
            announced_generation = s_connection_generation;
        }

        uint8_t opcode = 0u;
        uint16_t len = 0u;
        ret = app_ai_ws_wait_application_frame(APP_AI_WS_IO_TIMEOUT_MS, &opcode, &len);
        if (ret == 1) {
            continue;
        }
        if (ret != 0) {
            app_ai_ws_worker_close(ret);
            continue;
        }
        if (opcode != APP_AI_WS_OPCODE_TEXT) {
            continue;
        }

        app_ai_ws_event_t event;
        if (app_ai_ws_parse_event(s_rx_buffer, len, &event) != 0) {
            continue;
        }
        if (strcmp(event.type, "error") == 0) {
            APP_LOGW(TAG, "ai_ws event=server_error stage=result message=%s", event.error_message);
            return -52;
        }
        if (command->kind == APP_AI_WS_CMD_CAMERA) {
            if ((strcmp(event.type, "result") == 0 || strcmp(event.type, "camera_result") == 0) &&
                app_ai_ws_camera_result_is_terminal(&event)) {
                uint32_t copy_len = len < sizeof(command->camera_result) - 1u ?
                                    len : (uint32_t)sizeof(command->camera_result) - 1u;
                memcpy(command->camera_result, s_rx_buffer, copy_len);
                command->camera_result[copy_len] = '\0';
                command->camera_result_len = copy_len;
                return 0;
            }
            continue;
        }

        if (strcmp(event.type, "result") == 0) {
            app_ai_ws_copy_text(command->voice_result.status,
                                sizeof(command->voice_result.status),
                                event.status);
            if (event.answer_text[0] != '\0') {
                app_ai_ws_copy_text(command->voice_result.answer_text,
                                    sizeof(command->voice_result.answer_text),
                                    event.answer_text);
            }
            if (strcmp(event.status, "no_speech") == 0) {
                command->voice_result.no_speech = 1u;
                return 0;
            }
            if (strcmp(event.status, "cancelled") == 0 || strcmp(event.status, "canceled") == 0) {
                return APP_AI_WS_ERR_CANCELLED;
            }
            if (strcmp(event.status, "failed") == 0) {
                return -53;
            }
            if (strcmp(event.status, "audio_failed") == 0) {
                return command->voice_result.answer_text[0] != '\0' ? 0 : -54;
            }
            continue;
        }
        if (strcmp(event.type, "reply_ready") == 0) {
            if (!app_ai_ws_identifier_is_safe(event.session) ||
                strcmp(event.session, command->session) != 0) {
                return -55;
            }
            return app_ai_ws_download_reply(command, &event, deadline_ms);
        }
    }
    return APP_AI_WS_ERR_TIMEOUT;
}

static void app_ai_ws_save_pending_cancel(const app_ai_ws_command_t *command)
{
    if (command == NULL || s_state_mutex == NULL ||
        osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) != 0) {
        return;
    }
    app_ai_ws_copy_text(s_pending_cancel_request,
                        sizeof(s_pending_cancel_request),
                        command->request_id);
    app_ai_ws_copy_text(s_pending_cancel_session,
                        sizeof(s_pending_cancel_session),
                        command->session);
    osal_mutex_unlock(s_state_mutex);
}

static int app_ai_ws_process_command(app_ai_ws_command_t *command)
{
    uint32_t start_ms = osal_get_tick_ms();
    uint32_t deadline_ms = start_ms + APP_AI_PROCESS_TIMEOUT_MS;
    int ret = app_ai_ws_upload(command, deadline_ms);
    if (ret == 0) {
        ret = app_ai_ws_wait_result(command, deadline_ms);
    }
    if (ret == APP_AI_WS_ERR_CANCELLED) {
        app_ai_ws_save_pending_cancel(command);
    }
    APP_LOGI(TAG,
             "ai_ws_request event=done kind=%s request_id=%s session=%s ret=%d total_ms=%u",
             command->kind == APP_AI_WS_CMD_VOICE ? "voice" : "camera",
             command->request_id,
             command->session,
             ret,
             (unsigned int)(osal_get_tick_ms() - start_ms));
    return ret;
}

static int app_ai_ws_has_pending_control(void)
{
    int pending = 0;
    if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        pending = s_pending_cancel_request[0] != '\0' ||
                  s_pending_cancel_session[0] != '\0';
        osal_mutex_unlock(s_state_mutex);
    }
    return pending;
}

static int app_ai_ws_send_pending_control(void)
{
    char cancel_request[APP_AI_WS_REQUEST_ID_BYTES];
    char cancel_session[APP_AI_WS_SESSION_BYTES];
    cancel_request[0] = '\0';
    cancel_session[0] = '\0';
    if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        app_ai_ws_copy_text(cancel_request, sizeof(cancel_request), s_pending_cancel_request);
        app_ai_ws_copy_text(cancel_session, sizeof(cancel_session), s_pending_cancel_session);
        osal_mutex_unlock(s_state_mutex);
    }

    char json[320];
    int ret = 0;
    if (cancel_request[0] != '\0' || cancel_session[0] != '\0') {
        int len = snprintf(json,
                           sizeof(json),
                           "{\"type\":\"cancel\",\"request_id\":\"%s\",\"session\":\"%s\"}",
                           cancel_request,
                           cancel_session);
        ret = len > 0 && (size_t)len < sizeof(json) ? app_ai_ws_send_json(json) : -1;
        if (ret == 0 && s_state_mutex != NULL &&
            osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
            if (strcmp(s_pending_cancel_request, cancel_request) == 0 &&
                strcmp(s_pending_cancel_session, cancel_session) == 0) {
                s_pending_cancel_request[0] = '\0';
                s_pending_cancel_session[0] = '\0';
            }
            osal_mutex_unlock(s_state_mutex);
        }
    }
    return ret;
}

static void app_ai_ws_task(void *arg)
{
    (void)arg;
    uint32_t idle_reconnect_ms = APP_AI_WS_RECONNECT_MS;
    while (1) {
        app_ai_ws_command_t *command = NULL;
        if (osal_queue_recv(s_command_queue, &command, OSAL_WAIT_NONE) == 0 && command != NULL) {
            if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
                s_active_command = command;
                osal_mutex_unlock(s_state_mutex);
            }
            app_ai_ws_set_active_identity(command);
            command->ret = s_suspended != 0 ?
                           APP_AI_WS_ERR_CANCELLED : app_ai_ws_process_command(command);
            command->done = 1;
            if (s_state_mutex != NULL && osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
                if (s_active_command == command) {
                    s_active_command = NULL;
                }
                s_active_session[0] = '\0';
                osal_mutex_unlock(s_state_mutex);
            }
            continue;
        }

        if (s_suspended != 0 || service_network_is_ready() != 1) {
            if (s_sock >= 0) {
                app_ai_ws_worker_close(0);
            }
            s_force_reconnect = 0;
            idle_reconnect_ms = APP_AI_WS_RECONNECT_MS;
            (void)osal_task_notify_take(500u);
            continue;
        }

        if (s_connected == 0) {
            s_force_reconnect = 0;
            int ret = app_ai_ws_connect_once();
            if (ret != 0) {
                (void)osal_task_notify_take(idle_reconnect_ms);
                idle_reconnect_ms = idle_reconnect_ms < APP_AI_WS_RECONNECT_MAX_MS / 2u ?
                                    idle_reconnect_ms * 2u : APP_AI_WS_RECONNECT_MAX_MS;
                continue;
            }
            idle_reconnect_ms = APP_AI_WS_RECONNECT_MS;
        }

        if (app_ai_ws_has_pending_control()) {
            if (app_ai_ws_send_pending_control() != 0) {
                app_ai_ws_worker_close(-60);
            }
            continue;
        }

        uint8_t opcode = 0u;
        uint16_t len = 0u;
        int ret = app_ai_ws_wait_application_frame(100u, &opcode, &len);
        if (ret < 0) {
            app_ai_ws_worker_close(ret);
        }
    }
}

static int app_ai_ws_submit(app_ai_ws_command_t *command)
{
    if (!s_started || command == NULL || s_command_queue == NULL || s_suspended != 0) {
        return -1;
    }
    if (s_state_mutex == NULL || osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) != 0) {
        return -2;
    }
    if (s_operation_claimed != 0) {
        osal_mutex_unlock(s_state_mutex);
        return -3;
    }
    s_operation_claimed = 1;
    s_claimed_command = command;
    osal_mutex_unlock(s_state_mutex);

    app_ai_ws_command_t *queued = command;
    if (osal_queue_send(s_command_queue, &queued, 1000u) != 0) {
        if (osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
            if (s_claimed_command == command) {
                s_claimed_command = NULL;
            }
            s_operation_claimed = 0;
            osal_mutex_unlock(s_state_mutex);
        }
        return -4;
    }
    if (s_task != NULL) {
        (void)osal_task_notify_give(s_task);
    }
    while (command->done == 0) {
        osal_delay_ms(APP_AI_WS_WAIT_STEP_MS);
    }
    if (osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) == 0) {
        if (s_claimed_command == command) {
            s_claimed_command = NULL;
        }
        s_operation_claimed = 0;
        osal_mutex_unlock(s_state_mutex);
    }
    return command->ret;
}

int app_ai_ws_start(void)
{
    if (s_started) {
        return 0;
    }
    s_state_mutex = osal_mutex_create();
    s_command_queue = osal_queue_create(APP_AI_WS_QUEUE_DEPTH, sizeof(app_ai_ws_command_t *));
    s_rx_buffer = (uint8_t *)osal_heap_alloc_external(APP_AI_WS_RX_BUFFER_BYTES);
    s_tx_buffer = (uint8_t *)osal_heap_alloc_external(APP_AI_WS_TX_BUFFER_BYTES);
    s_wai_buffer = (uint8_t *)osal_heap_alloc_external(APP_AI_WS_FRAME_BYTES);
    if (s_state_mutex == NULL || s_command_queue == NULL ||
        s_rx_buffer == NULL || s_tx_buffer == NULL || s_wai_buffer == NULL) {
        APP_LOGE(TAG,
                 "ai_ws event=resource_fail psram_free=%u internal_free=%u",
                 (unsigned int)osal_heap_get_external_free_size(),
                 (unsigned int)osal_heap_get_internal_free_size());
        if (s_rx_buffer != NULL) {
            osal_heap_free(s_rx_buffer);
            s_rx_buffer = NULL;
        }
        if (s_tx_buffer != NULL) {
            osal_heap_free(s_tx_buffer);
            s_tx_buffer = NULL;
        }
        if (s_wai_buffer != NULL) {
            osal_heap_free(s_wai_buffer);
            s_wai_buffer = NULL;
        }
        if (s_command_queue != NULL) {
            osal_queue_delete(s_command_queue);
            s_command_queue = NULL;
        }
        if (s_state_mutex != NULL) {
            osal_mutex_delete(s_state_mutex);
            s_state_mutex = NULL;
        }
        return -1;
    }

    TaskHandle_t handle = NULL;
    BaseType_t ret = xTaskCreateWithCaps(app_ai_ws_task,
                                         "biz_ai_ws",
                                         APP_AI_WS_TASK_STACK,
                                         NULL,
                                         APP_AI_WS_TASK_PRIORITY,
                                         &handle,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        APP_LOGE(TAG,
                 "ai_ws event=task_start_fail ret=%d psram_free=%u internal_free=%u internal_largest=%u",
                 (int)ret,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        osal_heap_free(s_rx_buffer);
        osal_heap_free(s_tx_buffer);
        osal_heap_free(s_wai_buffer);
        s_rx_buffer = NULL;
        s_tx_buffer = NULL;
        s_wai_buffer = NULL;
        osal_queue_delete(s_command_queue);
        s_command_queue = NULL;
        osal_mutex_delete(s_state_mutex);
        s_state_mutex = NULL;
        return -2;
    }
    s_task = (osal_task_t)handle;
    s_started = 1;
    APP_LOGI(TAG,
             "ai_ws event=ready device=%s host=%s port=%d route=%s task_stack=%u psram_buffers=%u",
             APP_DEVICE_ID,
             APP_BUSINESS_SERVER_HOST,
             APP_BUSINESS_AI_WS_PORT,
             APP_BUSINESS_AI_WS_ROUTE,
             (unsigned int)APP_AI_WS_TASK_STACK,
             (unsigned int)(APP_AI_WS_RX_BUFFER_BYTES + APP_AI_WS_TX_BUFFER_BYTES +
                            APP_AI_WS_FRAME_BYTES));
    return 0;
}

int app_ai_ws_voice_request(const char *request_id,
                            const uint8_t *audio,
                            uint32_t audio_len,
                            const char sha256[APP_AI_WS_SHA256_TEXT_BYTES],
                            uint8_t *reply,
                            uint32_t reply_capacity,
                            app_ai_ws_voice_result_t *result)
{
    if (!app_ai_ws_identifier_is_safe(request_id) || audio == NULL || audio_len == 0u ||
        sha256 == NULL || strlen(sha256) != 64u || reply == NULL ||
        reply_capacity == 0u || result == NULL) {
        return -1;
    }
    app_ai_ws_command_t *command =
        (app_ai_ws_command_t *)osal_heap_alloc_external(sizeof(app_ai_ws_command_t));
    if (command == NULL) {
        return -2;
    }
    memset(command, 0, sizeof(*command));
    command->kind = APP_AI_WS_CMD_VOICE;
    command->input = audio;
    command->input_len = audio_len;
    command->reply = reply;
    command->reply_capacity = reply_capacity;
    app_ai_ws_copy_text(command->request_id, sizeof(command->request_id), request_id);
    app_ai_ws_copy_text(command->sha256, sizeof(command->sha256), sha256);
    int ret = app_ai_ws_submit(command);
    *result = command->voice_result;
    app_ai_ws_copy_text(result->session, sizeof(result->session), command->session);
    osal_heap_free(command);
    return ret;
}

int app_ai_ws_camera_request(const char *request_id,
                             const uint8_t *jpeg,
                             uint32_t jpeg_len,
                             const char sha256[APP_AI_WS_SHA256_TEXT_BYTES],
                             uint8_t *result_json,
                             uint32_t result_capacity,
                             uint32_t *result_len)
{
    if (!app_ai_ws_identifier_is_safe(request_id) || jpeg == NULL || jpeg_len == 0u ||
        sha256 == NULL || strlen(sha256) != 64u || result_json == NULL ||
        result_capacity < 2u || result_len == NULL) {
        return -1;
    }
    app_ai_ws_command_t *command =
        (app_ai_ws_command_t *)osal_heap_alloc_external(sizeof(app_ai_ws_command_t));
    if (command == NULL) {
        return -2;
    }
    memset(command, 0, sizeof(*command));
    command->kind = APP_AI_WS_CMD_CAMERA;
    command->input = jpeg;
    command->input_len = jpeg_len;
    app_ai_ws_copy_text(command->request_id, sizeof(command->request_id), request_id);
    app_ai_ws_copy_text(command->sha256, sizeof(command->sha256), sha256);
    int ret = app_ai_ws_submit(command);
    uint32_t copy_len = command->camera_result_len < result_capacity - 1u ?
                        command->camera_result_len : result_capacity - 1u;
    memcpy(result_json, command->camera_result, copy_len);
    result_json[copy_len] = '\0';
    *result_len = copy_len;
    osal_heap_free(command);
    return ret;
}

int app_ai_ws_cancel_active(void)
{
    int found = 0;
    if (!s_started || s_state_mutex == NULL ||
        osal_mutex_lock(s_state_mutex, OSAL_WAIT_FOREVER) != 0) {
        return -1;
    }
    app_ai_ws_command_t *command = s_active_command != NULL ?
                                   s_active_command : s_claimed_command;
    if (command != NULL && command->done == 0) {
        command->cancel = 1;
        app_ai_ws_copy_text(s_pending_cancel_request,
                            sizeof(s_pending_cancel_request),
                            command->request_id);
        app_ai_ws_copy_text(s_pending_cancel_session,
                            sizeof(s_pending_cancel_session),
                            command == s_active_command ? s_active_session : "");
        found = 1;
    }
    osal_mutex_unlock(s_state_mutex);
    if (found) {
        app_ai_ws_interrupt_socket();
        return 0;
    }
    return -2;
}

void app_ai_ws_network_changed(void)
{
    if (s_started) {
        app_ai_ws_interrupt_socket();
    }
}

void app_ai_ws_suspend(void)
{
    s_suspended = 1;
    (void)app_ai_ws_cancel_active();
    app_ai_ws_interrupt_socket();
}

void app_ai_ws_resume(void)
{
    s_suspended = 0;
    s_force_reconnect = 0;
    if (s_task != NULL) {
        (void)osal_task_notify_give(s_task);
    }
}

int app_ai_ws_is_ready(void)
{
    return s_connected != 0 ? 1 : 0;
}
