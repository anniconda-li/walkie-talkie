/**
 * @file bsp_ml307c.c
 * @brief ML307C-RTU/DTU 4G 模块驱动实现。
 */

#include "bsp_ml307c.h"

#include "bsp_common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bsp_ml307c";

#define ML307C_CRLF                 "\r\n"
#define ML307C_OK                   "OK"
#define ML307C_ERROR                "ERROR"
#define ML307C_CME_ERROR            "+CME ERROR:"
#define ML307C_DEFAULT_TIMEOUT_MS   3000u
#define ML307C_RX_BUFFER_SIZE       2048u
#define ML307C_LINE_MAX_LEN         256u
#define ML307C_DEFAULT_SOCKET_ID    1u
#define ML307C_SOCKET_MAX_ID        4u
#define ML307C_RESET_WAIT_MS        8000u
#define ML307C_REBOOT_READY_MS      30000u
#define ML307C_NET_READY_MS         120000u
#define ML307C_NET_POLL_MS          3000u
#define ML307C_SOCKET_READY_MS      120000u
#define ML307C_SOCKET_POLL_MS       3000u
#define ML307C_HTTP_TIMEOUT_MS      30000u
#define ML307C_HTTP_LATENCY_MS      100u
#define ML307C_HTTP_TASK_ID         1u
#define ML307C_HTTP_ROUTE           "6[1]"

struct ml307c_dev {
    ml307c_config_t config;
    ml307c_interface_t itf;
    ring_buffer_t rx_rb;
    uint8_t is_ready;
    uint8_t is_network_ok;
};

static void ml307c_log_response(const char *title, const char *resp);
static int ml307c_parse_first_int_after(const char *resp, const char *prefix, int *value);

static int ml307c_find_bytes(const uint8_t *buf, uint16_t len, const char *needle)
{
    if (buf == NULL || needle == NULL) {
        return -1;
    }

    uint16_t needle_len = (uint16_t)strlen(needle);
    if (needle_len == 0u || len < needle_len) {
        return -1;
    }

    for (uint16_t i = 0; i <= (uint16_t)(len - needle_len); i++) {
        if (memcmp(&buf[i], needle, needle_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static uint8_t ml307c_get_socket_id(ml307c_handle_t dev)
{
    if (dev->config.socket_id >= 1u && dev->config.socket_id <= ML307C_SOCKET_MAX_ID) {
        return dev->config.socket_id;
    }

    return ML307C_DEFAULT_SOCKET_ID;
}

static uint32_t ml307c_get_timeout(ml307c_handle_t dev)
{
    return dev->config.timeout_ms > 0u ? dev->config.timeout_ms : ML307C_DEFAULT_TIMEOUT_MS;
}

static void ml307c_clear_buffer(ml307c_handle_t dev)
{
    if (dev == NULL || dev->rx_rb.buf == NULL) {
        return;
    }

    memset(dev->rx_rb.buf, 0, dev->rx_rb.size);
    dev->rx_rb.head = 0;
    dev->rx_rb.tail = 0;
}

static void ml307c_drain_uart(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    uint8_t tmp[128];
    for (uint8_t i = 0; i < 3u; i++) {
        int rlen = dev->itf.uart_read(tmp, sizeof(tmp), 20u);
        if (rlen <= 0) {
            break;
        }

        tmp[rlen < (int)sizeof(tmp) ? rlen : ((int)sizeof(tmp) - 1)] = '\0';
        ml307c_log_response("ML307C 主动上报", (const char *)tmp);
    }
}

static int ml307c_append_response(ml307c_handle_t dev, const uint8_t *data, int len)
{
    if (dev == NULL || data == NULL || len <= 0 || dev->rx_rb.buf == NULL) {
        return -1;
    }

    uint16_t remain = (uint16_t)(dev->rx_rb.size - dev->rx_rb.head - 1u);
    uint16_t copy_len = (uint16_t)len;

    if (copy_len > remain) {
        copy_len = remain;
    }
    if (copy_len == 0u) {
        return 0;
    }

    memcpy(&dev->rx_rb.buf[dev->rx_rb.head], data, copy_len);
    dev->rx_rb.head = (uint16_t)(dev->rx_rb.head + copy_len);
    dev->rx_rb.buf[dev->rx_rb.head] = '\0';
    return copy_len;
}

static int ml307c_wait_response(ml307c_handle_t dev,
                                const char *expect,
                                uint32_t timeout_ms,
                                char *out,
                                uint16_t out_size)
{
    if (dev == NULL) {
        return -1;
    }

    uint32_t start = dev->itf.get_tick();
    int expect_found = (expect == NULL || expect[0] == '\0') ? 1 : 0;

    while ((dev->itf.get_tick() - start) < timeout_ms) {
        uint8_t tmp[128];
        int rlen = dev->itf.uart_read(tmp, sizeof(tmp), 100u);
        if (rlen > 0) {
            (void)ml307c_append_response(dev, tmp, rlen);

            const char *resp = (const char *)dev->rx_rb.buf;
            if (expect != NULL && expect[0] != '\0' && strstr(resp, expect) != NULL) {
                expect_found = 1;
            }
            if (strstr(resp, ML307C_CME_ERROR) != NULL || strstr(resp, ML307C_ERROR) != NULL) {
                if (out != NULL && out_size > 0u) {
                    snprintf(out, out_size, "%s", resp);
                }
                return -2;
            }
            if (expect_found && strstr(resp, ML307C_OK) != NULL) {
                if (out != NULL && out_size > 0u) {
                    snprintf(out, out_size, "%s", resp);
                }
                return 0;
            }
        }

        dev->itf.delay_ms(10u);
    }

    if (out != NULL && out_size > 0u && dev->rx_rb.buf != NULL) {
        snprintf(out, out_size, "%s", (const char *)dev->rx_rb.buf);
    }

    BSP_LOGW(TAG, "ML307C 等待响应超时, expect=%s",
             (expect != NULL && expect[0] != '\0') ? expect : ML307C_OK);
    return -3;
}

static int ml307c_send_cmd_capture(ml307c_handle_t dev,
                                   const char *cmd,
                                   const char *expect,
                                   uint32_t timeout_ms,
                                   char *out,
                                   uint16_t out_size)
{
    if (dev == NULL || cmd == NULL) {
        BSP_LOGE(TAG, "ML307C 发送命令参数无效, dev=%p, cmd=%p", dev, cmd);
        return -1;
    }

    ml307c_drain_uart(dev);
    ml307c_clear_buffer(dev);

    uint16_t len = (uint16_t)strlen(cmd);
    int written = dev->itf.uart_write((uint8_t *)cmd, len);
    if (written < 0 || written != len) {
        BSP_LOGE(TAG, "ML307C 命令发送失败, written=%d, len=%u",
                 written, (unsigned int)len);
        return -2;
    }

    BSP_LOGI(TAG, "ML307C 发送命令: %s", cmd);

    return ml307c_wait_response(dev,
                                expect != NULL ? expect : ML307C_OK,
                                timeout_ms,
                                out,
                                out_size);
}

static int ml307c_send_cmd(ml307c_handle_t dev,
                           const char *cmd,
                           const char *expect,
                           uint32_t timeout_ms)
{
    return ml307c_send_cmd_capture(dev, cmd, expect, timeout_ms, NULL, 0);
}

static void ml307c_log_response(const char *title, const char *resp)
{
    if (title == NULL || resp == NULL) {
        return;
    }

    char line[ML307C_LINE_MAX_LEN];
    uint16_t pos = 0;

    for (uint16_t i = 0; resp[i] != '\0' && pos < (sizeof(line) - 1u); i++) {
        if (resp[i] == '\r') {
            continue;
        }
        line[pos++] = resp[i] == '\n' ? '|' : resp[i];
    }

    line[pos] = '\0';
    BSP_LOGI(TAG, "%s: %s", title, line);
}

static int ml307c_wait_alive(ml307c_handle_t dev, uint32_t timeout_ms)
{
    if (dev == NULL) {
        return -1;
    }

    uint32_t start = dev->itf.get_tick();
    while ((dev->itf.get_tick() - start) < timeout_ms) {
        if (ml307c_check_alive(dev) == 0) {
            return 0;
        }

        dev->itf.delay_ms(1000u);
    }

    return -2;
}

static int ml307c_wait_network_link(ml307c_handle_t dev, uint32_t timeout_ms)
{
    if (dev == NULL) {
        return -1;
    }

    uint32_t start = dev->itf.get_tick();
    while ((dev->itf.get_tick() - start) < timeout_ms) {
        char resp[ML307C_LINE_MAX_LEN];
        int ret = ml307c_send_cmd_capture(dev,
                                          "AT+CEREG?" ML307C_CRLF,
                                          "+CEREG",
                                          ml307c_get_timeout(dev),
                                          resp,
                                          sizeof(resp));
        if (ret == 0) {
            ml307c_log_response("ML307C CEREG 响应", resp);
        }

        ret = ml307c_send_cmd_capture(dev,
                                      "AT+ISLINK?" ML307C_CRLF,
                                      "+ISLINK",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
        if (ret == 0) {
            int link = 0;
            ml307c_log_response("ML307C ISLINK 响应", resp);
            if (ml307c_parse_first_int_after(resp, "+ISLINK", &link) == 0 && link == 1) {
                dev->is_network_ok = 1u;
                BSP_LOGI(TAG, "ML307C 蜂窝数据网络已连接");
                return 0;
            }
        }

        dev->itf.delay_ms(ML307C_NET_POLL_MS);
    }

    dev->is_network_ok = 0u;
    BSP_LOGW(TAG, "ML307C 等待蜂窝数据网络连接超时");
    return -2;
}

static const char *ml307c_find_response_prefix(const char *resp, const char *prefix)
{
    if (resp == NULL || prefix == NULL) {
        return NULL;
    }

    const char *p = resp;
    while ((p = strstr(p, prefix)) != NULL) {
        if (p == resp || p[-1] == '\r' || p[-1] == '\n') {
            return p;
        }
        p++;
    }

    return NULL;
}

static int ml307c_parse_first_int_after(const char *resp, const char *prefix, int *value)
{
    if (resp == NULL || prefix == NULL || value == NULL) {
        return -1;
    }

    const char *p = ml307c_find_response_prefix(resp, prefix);
    if (p == NULL) {
        return -2;
    }

    p += strlen(prefix);
    while (*p == ' ' || *p == ':' || *p == '=') {
        p++;
    }

    *value = atoi(p);
    return 0;
}

static int ml307c_parse_quoted_value(const char *resp,
                                     const char *prefix,
                                     char *out,
                                     uint16_t out_size)
{
    if (resp == NULL || prefix == NULL || out == NULL || out_size == 0u) {
        return -1;
    }

    const char *p = ml307c_find_response_prefix(resp, prefix);
    if (p == NULL) {
        return -2;
    }

    p = strchr(p, '"');
    if (p == NULL) {
        return -3;
    }
    p++;

    const char *end = strchr(p, '"');
    if (end == NULL || end <= p) {
        return -4;
    }

    uint16_t len = (uint16_t)(end - p);
    if (len >= out_size) {
        len = (uint16_t)(out_size - 1u);
    }

    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int ml307c_parse_digits_after(const char *resp,
                                     const char *prefix,
                                     char *out,
                                     uint16_t out_size)
{
    if (resp == NULL || prefix == NULL || out == NULL || out_size == 0u) {
        return -1;
    }

    const char *p = ml307c_find_response_prefix(resp, prefix);
    if (p == NULL) {
        return -2;
    }

    p += strlen(prefix);
    while (*p == ' ' || *p == ':' || *p == '"' || *p == '=') {
        p++;
    }

    uint16_t i = 0;
    while (isdigit((unsigned char)*p) && i < (out_size - 1u)) {
        out[i++] = *p++;
    }

    out[i] = '\0';
    return i > 0u ? 0 : -3;
}

static int ml307c_parse_cereg_state(const char *resp, int *state)
{
    if (resp == NULL || state == NULL) {
        return -1;
    }

    const char *p = ml307c_find_response_prefix(resp, "+CEREG");
    if (p == NULL) {
        return -2;
    }

    p += strlen("+CEREG");
    while (*p == ' ' || *p == ':' || *p == '=') {
        p++;
    }

    int first = atoi(p);
    const char *comma = strchr(p, ',');
    if (comma != NULL) {
        *state = atoi(comma + 1);
    } else {
        *state = first;
    }

    return 0;
}

static int ml307c_parse_dtustate(const char *resp, uint8_t socket_id, int *state)
{
    if (resp == NULL || state == NULL) {
        return -1;
    }

    char prefix[24];
    snprintf(prefix, sizeof(prefix), "+DTUSTATE: %u,", (unsigned int)socket_id);
    const char *p = ml307c_find_response_prefix(resp, prefix);
    if (p == NULL) {
        snprintf(prefix, sizeof(prefix), "+DTUSTATE:%u,", (unsigned int)socket_id);
        p = ml307c_find_response_prefix(resp, prefix);
    }
    if (p == NULL) {
        return -2;
    }

    p = strchr(p, ',');
    if (p == NULL) {
        return -3;
    }
    p++;

    *state = atoi(p);
    return 0;
}

ml307c_handle_t ml307c_init(ml307c_config_t *cfg, ml307c_interface_t *itf)
{
    if (cfg == NULL || itf == NULL) {
        BSP_LOGE(TAG, "ML307C 初始化失败: 配置或接口为空");
        return NULL;
    }
    if (itf->uart_write == NULL || itf->uart_read == NULL ||
        itf->delay_ms == NULL || itf->get_tick == NULL) {
        BSP_LOGE(TAG, "ML307C 初始化失败: 必要接口函数为空");
        return NULL;
    }

    ml307c_handle_t dev = (ml307c_handle_t)calloc(1, sizeof(struct ml307c_dev));
    if (dev == NULL) {
        BSP_LOGE(TAG, "ML307C 初始化失败: 设备对象内存分配失败");
        return NULL;
    }

    dev->config = *cfg;
    dev->itf = *itf;
    dev->rx_rb.buf = (uint8_t *)calloc(1, ML307C_RX_BUFFER_SIZE);
    if (dev->rx_rb.buf == NULL) {
        free(dev);
        BSP_LOGE(TAG, "ML307C 初始化失败: 接收缓冲区内存分配失败");
        return NULL;
    }

    dev->rx_rb.size = ML307C_RX_BUFFER_SIZE;

    BSP_LOGI(TAG, "ML307C RTU 驱动初始化成功, timeout=%u, socket_id=%u",
             (unsigned int)ml307c_get_timeout(dev),
             (unsigned int)ml307c_get_socket_id(dev));
    return dev;
}

void ml307c_deinit(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    free(dev->rx_rb.buf);
    free(dev);
    BSP_LOGI(TAG, "ML307C 驱动已释放");
}

int ml307c_check_alive(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    int ret = ml307c_send_cmd(dev, "AT" ML307C_CRLF, ML307C_OK, ml307c_get_timeout(dev));
    if (ret == 0) {
        dev->is_ready = 1u;
    }

    return ret;
}

int ml307c_check_sim(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+ICCID" ML307C_CRLF,
                                      "+ICCID",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    char iccid[24];
    return ml307c_parse_digits_after(resp, "+ICCID", iccid, sizeof(iccid)) == 0 ? 0 : 1;
}

int ml307c_check_network(ml307c_handle_t dev)
{
    int state = 0;
    int ret = ml307c_get_network_state(dev, &state);
    if (ret != 0) {
        return ret;
    }

    if (state == 1 || state == 5) {
        dev->is_network_ok = 1u;
        return 0;
    }

    dev->is_network_ok = 0u;
    return 1;
}

int ml307c_get_network_state(ml307c_handle_t dev, int *state)
{
    if (dev == NULL || state == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+CEREG?" ML307C_CRLF,
                                      "+CEREG",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    ml307c_log_response("ML307C CEREG 响应", resp);
    return ml307c_parse_cereg_state(resp, state);
}

int ml307c_get_signal(ml307c_handle_t dev, int *rssi)
{
    if (dev == NULL || rssi == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+CSQ" ML307C_CRLF,
                                      "+CSQ",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return ml307c_parse_first_int_after(resp, "+CSQ", rssi);
}

int ml307c_get_operator(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    char resp[1024];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+SIMINFO" ML307C_CRLF,
                                      "OK",
                                      ml307c_get_timeout(dev) + 5000u,
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return ml307c_parse_quoted_value(resp, "carrier:", buf, 32u);
}

int ml307c_open_net(ml307c_handle_t dev)
{
    int link = 0;
    int ret = ml307c_get_link_state(dev, &link);
    if (ret != 0) {
        return ret;
    }

    if (link != 1) {
        dev->is_network_ok = 0u;
        return -2;
    }

    dev->is_network_ok = 1u;
    return 0;
}

int ml307c_get_link_state(ml307c_handle_t dev, int *link)
{
    if (dev == NULL || link == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+ISLINK?" ML307C_CRLF,
                                      "+ISLINK",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    ml307c_log_response("ML307C ISLINK 响应", resp);
    return ml307c_parse_first_int_after(resp, "+ISLINK", link);
}

int ml307c_close_net(ml307c_handle_t dev)
{
    return ml307c_tcp_close(dev);
}

static int ml307c_socket_connect(ml307c_handle_t dev, const char *ip, int port, int proto)
{
    if (dev == NULL || ip == NULL || port <= 0 || port > 65535 || proto < 0 || proto > 1) {
        return -1;
    }

    uint8_t socket_id = ml307c_get_socket_id(dev);
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "AT+DTUTASK=%u,1,\"SOCK\"" ML307C_CRLF, (unsigned int)socket_id);
    int ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+SOCK=%u,\"%s\",%d,%d" ML307C_CRLF,
             (unsigned int)socket_id, ip, port, proto);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+DTUPSUP=1,\"%u\"" ML307C_CRLF, (unsigned int)socket_id);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+DTUPSDN=%u,\"6[1]\"" ML307C_CRLF, (unsigned int)socket_id);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    ret = ml307c_send_cmd(dev, "AT+RESET" ML307C_CRLF, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        BSP_LOGW(TAG, "ML307C 复位命令未收到 OK，继续等待模块重启, ret=%d", ret);
    }

    dev->itf.delay_ms(ML307C_RESET_WAIT_MS);
    ret = ml307c_wait_alive(dev, ML307C_REBOOT_READY_MS);
    if (ret != 0) {
        BSP_LOGW(TAG, "ML307C 重启后 AT 未恢复, ret=%d", ret);
        return ret;
    }

    ret = ml307c_wait_network_link(dev, ML307C_NET_READY_MS);
    if (ret != 0) {
        return ret;
    }

    uint32_t start = dev->itf.get_tick();
    uint32_t timeout = ml307c_get_timeout(dev) * 10u;
    if (timeout < ML307C_SOCKET_READY_MS) {
        timeout = ML307C_SOCKET_READY_MS;
    }

    while ((dev->itf.get_tick() - start) < timeout) {
        char resp[ML307C_LINE_MAX_LEN];
        snprintf(cmd, sizeof(cmd), "AT+DTUSTATE=%u" ML307C_CRLF, (unsigned int)socket_id);
        ret = ml307c_send_cmd_capture(dev,
                                      cmd,
                                      "+DTUSTATE",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
        if (ret == 0) {
            int state = 0;
            ml307c_log_response("ML307C DTUSTATE 响应", resp);
            if (ml307c_parse_dtustate(resp, socket_id, &state) == 0) {
                BSP_LOGI(TAG, "ML307C socket 通道状态, id=%u, state=%d",
                         (unsigned int)socket_id, state);
                if (state == 1) {
                    BSP_LOGI(TAG, "ML307C socket 通道已连接, id=%u",
                             (unsigned int)socket_id);
                    return 0;
                }
            } else {
                BSP_LOGW(TAG, "ML307C DTUSTATE 响应解析失败");
            }
        }

        dev->itf.delay_ms(ML307C_SOCKET_POLL_MS);
    }

    BSP_LOGW(TAG, "ML307C socket 通道连接超时, id=%u", (unsigned int)socket_id);
    return -2;
}

int ml307c_tcp_connect(ml307c_handle_t dev, const char *ip, int port)
{
    return ml307c_socket_connect(dev, ip, port, 0);
}

int ml307c_udp_connect(ml307c_handle_t dev, const char *ip, int port)
{
    return ml307c_socket_connect(dev, ip, port, 1);
}

static int ml307c_send_route(ml307c_handle_t dev, const char *route, uint8_t *data, int len)
{
    if (dev == NULL || (data == NULL && len > 0) || len < 0 || len > 65535) {
        return -1;
    }

    if (route == NULL || route[0] == '\0') {
        return -1;
    }

    char prefix[32];
    int prefix_len = snprintf(prefix,
                              sizeof(prefix),
                              "AT+SENDR=\"%s\",",
                              route);
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix)) {
        return -2;
    }

    ml307c_drain_uart(dev);
    ml307c_clear_buffer(dev);

    if (dev->itf.uart_write((uint8_t *)prefix, (uint16_t)prefix_len) != prefix_len) {
        return -3;
    }
    if (len > 0 && dev->itf.uart_write(data, (uint16_t)len) != len) {
        return -4;
    }
    if (dev->itf.uart_write((uint8_t *)ML307C_CRLF, 2u) != 2) {
        return -5;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_wait_response(dev,
                                   "+SENDR",
                                   ml307c_get_timeout(dev),
                                   resp,
                                   sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    int send_ret = -1;
    if (ml307c_parse_first_int_after(resp, "+SENDR", &send_ret) != 0) {
        return -6;
    }

    return send_ret == 0 ? 0 : -7;
}

int ml307c_tcp_send(ml307c_handle_t dev, uint8_t *data, int len)
{
    uint8_t socket_id = ml307c_get_socket_id(dev);
    char route[8];
    snprintf(route, sizeof(route), "%u[1]", (unsigned int)socket_id);
    return ml307c_send_route(dev, route, data, len);
}

int ml307c_udp_send(ml307c_handle_t dev, uint8_t *data, int len)
{
    uint8_t socket_id = ml307c_get_socket_id(dev);
    char route[8];
    snprintf(route, sizeof(route), "%u[1]", (unsigned int)socket_id);
    return ml307c_send_route(dev, route, data, len);
}

int ml307c_read_raw(ml307c_handle_t dev, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (dev == NULL || buf == NULL || len == 0u) {
        return -1;
    }

    return dev->itf.uart_read(buf, len, timeout_ms);
}

static int ml307c_http_capture_response(ml307c_handle_t dev,
                                        uint8_t *resp,
                                        uint16_t resp_size,
                                        uint16_t *resp_len,
                                        uint32_t timeout_ms)
{
    uint32_t start = dev->itf.get_tick();
    uint16_t total = 0u;
    int http_pos = -1;

    while ((dev->itf.get_tick() - start) < timeout_ms) {
        uint8_t tmp[128];
        int rlen = dev->itf.uart_read(tmp, sizeof(tmp), 100u);
        if (rlen > 0) {
            uint16_t copy_len = (uint16_t)rlen;
            if (copy_len > (uint16_t)(resp_size - total)) {
                copy_len = (uint16_t)(resp_size - total);
            }
            if (copy_len > 0u) {
                memcpy(&resp[total], tmp, copy_len);
                total = (uint16_t)(total + copy_len);
            }

            http_pos = ml307c_find_bytes(resp, total, "\r\n+HTTP:");
            if (http_pos < 0) {
                http_pos = ml307c_find_bytes(resp, total, "+HTTP:");
            }
            if (http_pos >= 0 && ml307c_find_bytes(&resp[http_pos], (uint16_t)(total - http_pos), "\r\nOK") >= 0) {
                int body_start = 0;
                while (body_start < http_pos &&
                       (resp[body_start] == '\r' || resp[body_start] == '\n')) {
                    body_start++;
                }

                int body_len = http_pos - body_start;
                if (body_len < 0) {
                    body_len = 0;
                }
                if (body_len > 0 && body_start > 0) {
                    memmove(resp, &resp[body_start], (size_t)body_len);
                }

                *resp_len = (uint16_t)body_len;
                return 0;
            }
        }

        dev->itf.delay_ms(10u);
    }

    *resp_len = total;
    BSP_LOGW(TAG, "ML307C HTTP 响应等待超时, received=%u", (unsigned int)total);
    return -1;
}

int ml307c_http_post(ml307c_handle_t dev,
                     uint8_t id,
                     const char *url,
                     const char *header,
                     const uint8_t *body,
                     uint16_t body_len,
                     uint8_t *resp,
                     uint16_t resp_size,
                     uint16_t *resp_len)
{
    if (dev == NULL || url == NULL || body == NULL || body_len == 0u ||
        resp == NULL || resp_size == 0u || resp_len == NULL) {
        return -1;
    }

    if (id == 0u) {
        id = ML307C_HTTP_TASK_ID;
    }

    const char *safe_header = header != NULL ? header : "";
    char cmd[768];

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPURL=%u,1,1,\"%s\",\"%s\"" ML307C_CRLF,
             (unsigned int)id,
             safe_header,
             url);
    int ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPCFG=%u,10,30,0,0" ML307C_CRLF,
             (unsigned int)id);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPSSL=%u,%d,1" ML307C_CRLF,
             (unsigned int)id,
             strncmp(url, "https://", 8u) == 0 ? 1 : 0);
    (void)ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPRESP=%u,0,1,\"%s\"" ML307C_CRLF,
             (unsigned int)id,
             ML307C_HTTP_ROUTE);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        return ret;
    }

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTP=%u,%u,%u,%u" ML307C_CRLF,
             (unsigned int)id,
             (unsigned int)body_len,
             (unsigned int)ML307C_HTTP_TIMEOUT_MS,
             (unsigned int)ML307C_HTTP_LATENCY_MS);

    ml307c_drain_uart(dev);
    ml307c_clear_buffer(dev);

    uint16_t cmd_len = (uint16_t)strlen(cmd);
    if (dev->itf.uart_write((uint8_t *)cmd, cmd_len) != cmd_len) {
        return -2;
    }
    if (dev->itf.uart_write((uint8_t *)body, body_len) != body_len) {
        return -3;
    }

    *resp_len = 0u;
    return ml307c_http_capture_response(dev,
                                        resp,
                                        resp_size,
                                        resp_len,
                                        ML307C_HTTP_TIMEOUT_MS + 10000u);
}

int ml307c_tcp_close(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    uint8_t socket_id = ml307c_get_socket_id(dev);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+DTUTASK=%u,0,\"SOCK\"" ML307C_CRLF, (unsigned int)socket_id);
    return ml307c_send_cmd(dev, cmd, "+DTUTASK", ml307c_get_timeout(dev));
}

int ml307c_send_sms(ml307c_handle_t dev, const char *num, const char *msg)
{
    (void)dev;
    (void)num;
    (void)msg;
    BSP_LOGW(TAG, "当前 ML307C RTU 文档未提供 SMS 指令，本接口暂不支持");
    return -1;
}

int ml307c_get_imei(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+IMEI" ML307C_CRLF,
                                      "+IMEI",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return ml307c_parse_quoted_value(resp, "+IMEI", buf, 16u);
}

int ml307c_get_iccid(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_send_cmd_capture(dev,
                                      "AT+ICCID" ML307C_CRLF,
                                      "+ICCID",
                                      ml307c_get_timeout(dev),
                                      resp,
                                      sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return ml307c_parse_digits_after(resp, "+ICCID", buf, 21u);
}
