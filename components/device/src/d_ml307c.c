/**
 * @file d_ml307c.c
 * @brief ML307C-RTU/DTU 4G 模块驱动实现。
 */

#include "d_ml307c.h"

#include "d_config.h"
#include "osal_mutex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "d_ml307c";

#define ML307C_CRLF                 "\r\n"        /**< AT 命令行结束符。 */
#define ML307C_OK                   "OK"          /**< AT 成功响应关键字。 */
#define ML307C_ERROR                "ERROR"       /**< AT 普通错误响应关键字。 */
#define ML307C_CME_ERROR            "+CME ERROR:" /**< AT CME 错误响应前缀。 */
#define ML307C_DEFAULT_TIMEOUT_MS   3000u         /**< AT 命令默认超时时间。 */
#define ML307C_RX_BUFFER_SIZE       2048u         /**< AT 响应接收缓存大小。 */
#define ML307C_DOWNLINK_BUFFER_SIZE 4096u         /**< AT 等待期间暂存的 DTU 下行数据。 */
#define ML307C_LINE_MAX_LEN         256u          /**< 单条 AT 响应日志/解析缓存长度。 */
#define ML307C_DEFAULT_SOCKET_ID    1u            /**< 默认 DTU socket 通道号。 */
#define ML307C_SOCKET_MAX_ID        4u            /**< ML307C 支持的最大 socket 通道号。 */
#define ML307C_RESET_WAIT_MS        8000u         /**< 发送复位命令后的固定等待时间。 */
#define ML307C_REBOOT_READY_MS      30000u        /**< 模块复位后等待 AT 恢复的最长时间。 */
#define ML307C_NET_READY_MS         120000u       /**< 等待蜂窝网络就绪的最长时间。 */
#define ML307C_NET_POLL_MS          3000u         /**< 蜂窝网络状态轮询间隔。 */
#define ML307C_SOCKET_READY_MS      120000u       /**< 等待 socket 通道就绪的最长时间。 */
#define ML307C_SOCKET_POLL_MS       3000u         /**< socket 通道状态轮询间隔。 */
#define ML307C_HTTP_TIMEOUT_MS      30000u        /**< HTTP 默认超时时间。 */
#define ML307C_HTTP_LATENCY_MS      100u          /**< HTTP 命令中配置的发送延迟。 */
#define ML307C_HTTP_TASK_ID         1u            /**< HTTP 任务默认 ID。 */
#define ML307C_HTTP_TASK_MAX_ID     5u            /**< 手册定义的最大 HTTP 通道 ID。 */
#define ML307C_HTTP_ROUTE           "6[1]"        /**< HTTP 响应路由标识。 */
#define ML307C_HTTP_METHOD_GET      0u            /**< AT+HTTPURL 请求方法：按手册示例 0 表示 GET。 */
#define ML307C_HTTP_METHOD_POST     1u            /**< AT+HTTPURL 请求方法：按手册示例 1 表示 POST。 */
#define ML307C_HTTP_CONN_TIMEOUT_S  3u            /**< AT+HTTPCFG 连接超时，手册范围 0-10 秒。 */
#define ML307C_HTTP_RSP_TIMEOUT_S   5u            /**< AT+HTTPCFG 响应超时，手册范围 0-10 秒。 */
#define ML307C_HTTP_DNS_IPV4_FIRST  0u            /**< AT+HTTPCFG DNS 优先级：IPv4 优先。 */
#define ML307C_HTTP_ENCODE_NONE     0u            /**< AT+HTTPCFG URL 不编码。 */
#define ML307C_HTTP_RECOVER_GAP_MS  500u          /**< HTTP 实例完成后给模块释放资源的间隔。 */
#define D_ML307C_LOCK_TIMEOUT_MS    5000u         /**< ML307C 互斥锁默认等待时间。 */
#define ML307C_TIMEOUT_SNIPPET_LEN  160u          /**< AT 超时日志中保留的响应摘要长度。 */
#define ML307C_ALIVE_ATTEMPT_MS     2000u         /**< 单次 AT 存活检测等待时间。 */
#define ML307C_ALIVE_RETRY_GAP_MS   300u          /**< AT 存活检测失败后的重试间隔。 */
#define ML307C_STATUS_CACHE_MS      15000u        /**< 状态查询缓存时间，降低 UI 轮询 AT 频率。 */

/**
 * @brief ML307C 内部依赖的底层能力函数表。
 */
typedef struct {
    int (*uart_write)(uint8_t *data, uint16_t len);
    int (*uart_read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    void (*delay_ms)(uint32_t ms);
    uint32_t (*get_tick)(void);
} ml307c_interface_t;

/**
 * @brief 简单线性接收缓存状态。
 */
typedef struct {
    uint8_t *buf;
    uint16_t size;
    uint16_t head;
    uint16_t tail;
} ring_buffer_t;

/**
 * @brief ML307C 驱动内部设备状态。
 */
struct ml307c_dev {
    ml307c_config_t config;
    ml307c_interface_t itf;
    ring_buffer_t rx_rb;
    uint8_t is_ready;
    uint8_t is_network_ok;
};

/** @brief 单例 ML307C 设备对象。 */
static struct ml307c_dev s_ml307c_dev;

/** @brief 单例 ML307C AT 响应接收缓存。 */
static uint8_t s_ml307c_rx_buf[ML307C_RX_BUFFER_SIZE];

/** @brief AT 命令等待期间抢读到的 DTU 下行数据。 */
static uint8_t s_ml307c_downlink_buf[ML307C_DOWNLINK_BUFFER_SIZE];

/** @brief 暂存下行数据长度。 */
static uint16_t s_ml307c_downlink_len = 0u;

/** @brief 当前已初始化的 ML307C 设备指针。 */
static struct ml307c_dev *s_ml307c = NULL;

/** @brief 保护 ML307C AT 命令串行访问的互斥锁。 */
static osal_mutex_t s_ml307c_mutex = NULL;

/** @brief TCP 通道是否已由本层标记为连接。 */
static uint8_t s_ml307c_tcp_connected = 0u;

/** @brief UDP 通道是否已由本层标记为连接。 */
static uint8_t s_ml307c_udp_connected = 0u;

/** @brief 最近一次主动 AT 查询得到的网络状态。DTU 数据期避免继续打 AT。 */
static d_ml307c_status_t s_ml307c_cached_status;

/** @brief 缓存状态是否有效。 */
static uint8_t s_ml307c_cached_status_valid = 0u;

/** @brief 最近一次状态缓存时间。 */
static uint32_t s_ml307c_cached_status_tick = 0u;

/** @brief 下次使用的 HTTP 通道 ID，轮换以规避模块复用同一通道时的状态残留。 */
static uint8_t s_ml307c_http_next_id = ML307C_HTTP_TASK_ID;

static void ml307c_log_response(const char *title, const char *resp);
static void ml307c_log_timeout_summary(const char *cmd,
                                       const char *expect,
                                       const uint8_t *data,
                                       uint16_t len);
static int ml307c_uart_write_all(struct ml307c_dev *dev,
                                 const uint8_t *data,
                                 uint32_t len,
                                 uint32_t timeout_ms);
static int ml307c_wait_token(struct ml307c_dev *dev,
                             const char *cmd,
                             const char *token,
                             uint32_t timeout_ms);
static int ml307c_parse_first_int_after(const char *resp, const char *prefix, int *value);
static uint8_t d_ml307c_alloc_http_id(void);
static struct ml307c_dev * ml307c_init(ml307c_config_t *cfg, ml307c_interface_t *itf);
static void ml307c_deinit(struct ml307c_dev * dev);
static int ml307c_check_alive(struct ml307c_dev * dev);
static int ml307c_check_sim(struct ml307c_dev * dev);
static int ml307c_get_network_state(struct ml307c_dev * dev, int *state);
static int ml307c_get_signal(struct ml307c_dev * dev, int *rssi);
static int ml307c_get_link_state(struct ml307c_dev * dev, int *link);
static int ml307c_tcp_connect(struct ml307c_dev * dev, const char *ip, int port);
static int ml307c_udp_connect(struct ml307c_dev * dev, const char *ip, int port);
static int ml307c_tcp_send(struct ml307c_dev * dev, uint8_t *data, int len);
static int ml307c_udp_send(struct ml307c_dev * dev, uint8_t *data, int len);
static int ml307c_read_raw(struct ml307c_dev * dev, uint8_t *buf, uint16_t len, uint32_t timeout_ms);
static int ml307c_http_post(struct ml307c_dev * dev,
                            uint8_t id,
                            const char *url,
                            const char *header,
                            const uint8_t *body,
                            uint16_t body_len,
                            uint8_t *resp,
                            uint16_t resp_size,
                            uint16_t *resp_len,
                            uint32_t timeout_ms);
static int ml307c_tcp_close(struct ml307c_dev * dev);
static int d_ml307c_lock(uint32_t timeout_ms);
static void d_ml307c_unlock(void);
static int ml307c_response_has_line(const char *resp, const char *line);
static void ml307c_stash_downlink(const uint8_t *data, uint16_t len);
static int ml307c_pop_downlink(uint8_t *buf, uint16_t len);
static void d_ml307c_cache_status(const d_ml307c_status_t *status);
static void d_ml307c_fill_ready_status(d_ml307c_status_t *status);

/**
 * @brief 在字节缓存中查找指定文本片段。
 */
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

/**
 * @brief 获取有效 socket 通道号，配置非法时回退默认通道。
 */
static uint8_t ml307c_get_socket_id(struct ml307c_dev * dev)
{
    if (dev->config.socket_id >= 1u && dev->config.socket_id <= ML307C_SOCKET_MAX_ID) {
        return dev->config.socket_id;
    }

    return ML307C_DEFAULT_SOCKET_ID;
}

/**
 * @brief 获取 AT 命令超时时间，配置为 0 时使用默认值。
 */
static uint32_t ml307c_get_timeout(struct ml307c_dev * dev)
{
    return dev->config.timeout_ms > 0u ? dev->config.timeout_ms : ML307C_DEFAULT_TIMEOUT_MS;
}

/**
 * @brief 清空 ML307C AT 响应缓存。
 */
static void ml307c_clear_buffer(struct ml307c_dev * dev)
{
    if (dev == NULL || dev->rx_rb.buf == NULL) {
        return;
    }

    memset(dev->rx_rb.buf, 0, dev->rx_rb.size);
    dev->rx_rb.head = 0;
    dev->rx_rb.tail = 0;
}

static void ml307c_stash_downlink(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0u) {
        return;
    }

    if (len > sizeof(s_ml307c_downlink_buf)) {
        data = &data[len - sizeof(s_ml307c_downlink_buf)];
        len = sizeof(s_ml307c_downlink_buf);
    }

    uint16_t free_len = (uint16_t)(sizeof(s_ml307c_downlink_buf) - s_ml307c_downlink_len);
    if (len > free_len) {
        uint16_t drop_len = (uint16_t)(len - free_len);
        if (drop_len >= s_ml307c_downlink_len) {
            s_ml307c_downlink_len = 0u;
        } else {
            memmove(s_ml307c_downlink_buf,
                    &s_ml307c_downlink_buf[drop_len],
                    (size_t)(s_ml307c_downlink_len - drop_len));
            s_ml307c_downlink_len = (uint16_t)(s_ml307c_downlink_len - drop_len);
        }
    }

    memcpy(&s_ml307c_downlink_buf[s_ml307c_downlink_len], data, len);
    s_ml307c_downlink_len = (uint16_t)(s_ml307c_downlink_len + len);
}

static int ml307c_pop_downlink(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0u || s_ml307c_downlink_len == 0u) {
        return 0;
    }

    uint16_t copy_len = s_ml307c_downlink_len < len ? s_ml307c_downlink_len : len;
    memcpy(buf, s_ml307c_downlink_buf, copy_len);
    if (copy_len < s_ml307c_downlink_len) {
        memmove(s_ml307c_downlink_buf,
                &s_ml307c_downlink_buf[copy_len],
                (size_t)(s_ml307c_downlink_len - copy_len));
    }
    s_ml307c_downlink_len = (uint16_t)(s_ml307c_downlink_len - copy_len);
    return (int)copy_len;
}

/**
 * @brief 读取并丢弃串口中残留的主动上报数据。
 */
static void ml307c_drain_uart(struct ml307c_dev * dev)
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

        if (ml307c_find_bytes(tmp, (uint16_t)rlen, "WTK1") >= 0) {
            ml307c_stash_downlink(tmp, (uint16_t)rlen);
            continue;
        }

        tmp[rlen < (int)sizeof(tmp) ? rlen : ((int)sizeof(tmp) - 1)] = '\0';
        ml307c_log_response("ML307C 主动上报", (const char *)tmp);
    }
}

static int ml307c_uart_write_all(struct ml307c_dev *dev,
                                 const uint8_t *data,
                                 uint32_t len,
                                 uint32_t timeout_ms)
{
    if (dev == NULL || (data == NULL && len > 0u)) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (timeout_ms == 0u) {
        timeout_ms = ml307c_get_timeout(dev);
    }

    uint32_t start = dev->itf.get_tick();
    uint32_t offset = 0u;
    while (offset < len) {
        uint32_t remain = len - offset;
        uint16_t write_len = remain > 512u ? 512u : (uint16_t)remain;
        int written = dev->itf.uart_write((uint8_t *)&data[offset], write_len);
        if (written > 0) {
            uint16_t accepted = written > (int)write_len ? write_len : (uint16_t)written;
            offset += (uint32_t)accepted;
            continue;
        }
        if ((dev->itf.get_tick() - start) >= timeout_ms) {
            D_LOGW(TAG,
                   "ML307C UART 写入超时, written=%u, total=%u, last=%d",
                   (unsigned int)offset,
                   (unsigned int)len,
                   written);
            return -2;
        }
        dev->itf.delay_ms(10u);
    }

    return 0;
}

/**
 * @brief 将串口读取到的数据追加到 AT 响应缓存。
 */
static int ml307c_append_response(struct ml307c_dev * dev, const uint8_t *data, int len)
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

static int ml307c_wait_token(struct ml307c_dev *dev,
                             const char *cmd,
                             const char *token,
                             uint32_t timeout_ms)
{
    if (dev == NULL || token == NULL || token[0] == '\0') {
        return -1;
    }
    if (timeout_ms == 0u) {
        timeout_ms = ml307c_get_timeout(dev);
    }

    uint32_t start = dev->itf.get_tick();
    while ((dev->itf.get_tick() - start) < timeout_ms) {
        uint8_t tmp[128];
        int rlen = dev->itf.uart_read(tmp, sizeof(tmp), 100u);
        if (rlen > 0) {
            (void)ml307c_append_response(dev, tmp, rlen);
            const char *resp = (const char *)dev->rx_rb.buf;
            if (strstr(resp, token) != NULL) {
                return 0;
            }
            if (strstr(resp, ML307C_CME_ERROR) != NULL ||
                ml307c_response_has_line(resp, ML307C_ERROR)) {
                return -2;
            }
        }
        dev->itf.delay_ms(10u);
    }

    ml307c_log_timeout_summary(cmd != NULL ? cmd : "wait token",
                               token,
                               dev->rx_rb.buf,
                               dev->rx_rb.head);
    return -3;
}

/**
 * @brief 判断响应中是否存在独立一行目标文本。
 */
static int ml307c_response_has_line(const char *resp, const char *line)
{
    if (resp == NULL || line == NULL || line[0] == '\0') {
        return 0;
    }

    size_t line_len = strlen(line);
    const char *p = resp;
    while ((p = strstr(p, line)) != NULL) {
        char before = (p == resp) ? '\n' : p[-1];
        char after = p[line_len];
        int before_ok = (before == '\r' || before == '\n');
        int after_ok = (after == '\0' || after == '\r' || after == '\n');
        if (before_ok && after_ok) {
            return 1;
        }
        p += line_len;
    }

    return 0;
}

/**
 * @brief 等待 AT 命令响应并捕获完整响应文本。
 */
static int ml307c_wait_response(struct ml307c_dev * dev,
                                const char *cmd,
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
    uint16_t downlink_len_at_start = s_ml307c_downlink_len;

    while ((dev->itf.get_tick() - start) < timeout_ms) {
        uint8_t tmp[128];
        int rlen = dev->itf.uart_read(tmp, sizeof(tmp), 100u);
        if (rlen > 0) {
            if ((ml307c_find_bytes(tmp, (uint16_t)rlen, "WTK1") >= 0 ||
                 s_ml307c_downlink_len != downlink_len_at_start) &&
                ml307c_find_bytes(tmp, (uint16_t)rlen, expect) < 0 &&
                ml307c_find_bytes(tmp, (uint16_t)rlen, ML307C_OK) < 0 &&
                ml307c_find_bytes(tmp, (uint16_t)rlen, ML307C_ERROR) < 0) {
                ml307c_stash_downlink(tmp, (uint16_t)rlen);
                continue;
            }

            (void)ml307c_append_response(dev, tmp, rlen);

            const char *resp = (const char *)dev->rx_rb.buf;
            if (expect != NULL && expect[0] != '\0' && strstr(resp, expect) != NULL) {
                expect_found = 1;
            }
            if (strstr(resp, ML307C_CME_ERROR) != NULL ||
                ml307c_response_has_line(resp, ML307C_ERROR)) {
                if (out != NULL && out_size > 0u) {
                    snprintf(out, out_size, "%s", resp);
                }
                return -2;
            }
            if (expect_found && ml307c_response_has_line(resp, ML307C_OK)) {
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

    ml307c_log_timeout_summary(cmd != NULL ? cmd : "raw data", expect, dev->rx_rb.buf, dev->rx_rb.head);
    return -3;
}

/**
 * @brief 发送 AT 命令并可选捕获响应内容。
 */
static int ml307c_send_cmd_capture(struct ml307c_dev * dev,
                                   const char *cmd,
                                   const char *expect,
                                   uint32_t timeout_ms,
                                   char *out,
                                   uint16_t out_size)
{
    if (dev == NULL || cmd == NULL) {
        D_LOGE(TAG, "ML307C 发送命令参数无效, dev=%p, cmd=%p", dev, cmd);
        return -1;
    }

    ml307c_drain_uart(dev);
    ml307c_clear_buffer(dev);

    uint16_t len = (uint16_t)strlen(cmd);
    int written = dev->itf.uart_write((uint8_t *)cmd, len);
    if (written < 0 || written != len) {
        D_LOGE(TAG, "ML307C 命令发送失败, written=%d, len=%u",
                 written, (unsigned int)len);
        return -2;
    }

    return ml307c_wait_response(dev,
                                cmd,
                                expect != NULL ? expect : ML307C_OK,
                                timeout_ms,
                                out,
                                out_size);
}

/**
 * @brief 发送 AT 命令并只检查期望响应。
 */
static int ml307c_send_cmd(struct ml307c_dev * dev,
                           const char *cmd,
                           const char *expect,
                           uint32_t timeout_ms)
{
    return ml307c_send_cmd_capture(dev, cmd, expect, timeout_ms, NULL, 0);
}

/**
 * @brief 将多行 AT 响应整理成单行日志输出。
 */
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
    D_LOGI(TAG, "%s: %s", title, line);
}

/**
 * @brief 输出 AT 等待超时时的命令、期望响应和接收摘要。
 */
static void ml307c_log_timeout_summary(const char *cmd,
                                       const char *expect,
                                       const uint8_t *data,
                                       uint16_t len)
{
    const char *cmd_text = (cmd != NULL && cmd[0] != '\0') ? cmd : "unknown";
    const char *expect_text = (expect != NULL && expect[0] != '\0') ? expect : ML307C_OK;
    char clean_cmd[64];
    uint16_t cmd_pos = 0;
    for (uint16_t i = 0; cmd_text[i] != '\0' && cmd_pos < (sizeof(clean_cmd) - 1u); i++) {
        char ch = cmd_text[i];
        if (ch == '\r' || ch == '\n') {
            break;
        }
        clean_cmd[cmd_pos++] = ch;
    }
    clean_cmd[cmd_pos] = '\0';

    if (data == NULL || len == 0u) {
        D_LOGW(TAG,
                    "ML307C AT 超时: cmd=\"%s\", expect=\"%s\", 未收到任何字节。可能原因: 模块未上电/未开机、TX/RX 未交叉、引脚配置错误、波特率不匹配。",
                    clean_cmd,
                    expect_text);
        return;
    }

    char snippet[ML307C_TIMEOUT_SNIPPET_LEN + 1u];
    uint16_t copy_len = len;
    if (copy_len > ML307C_TIMEOUT_SNIPPET_LEN) {
        copy_len = ML307C_TIMEOUT_SNIPPET_LEN;
    }

    uint16_t pos = 0;
    for (uint16_t i = 0; i < copy_len && pos < ML307C_TIMEOUT_SNIPPET_LEN; i++) {
        char ch = (char)data[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            ch = '|';
        } else if (!isprint((unsigned char)ch)) {
            ch = '.';
        }
        snippet[pos++] = ch;
    }
    snippet[pos] = '\0';

    const char *reason = "收到数据但没有目标响应，可能是模块未进入 AT 模式、命令不被支持、响应格式和驱动预期不一致。";
    if (strstr(snippet, "I (") != NULL ||
        strstr(snippet, "wdriver_") != NULL ||
        strstr(snippet, "d_") != NULL ||
        strstr(snippet, "walkie_app") != NULL) {
        reason = "串口收到的是 ESP 自己的日志，不是 ML307C 回包；优先检查 UART 引脚是否接到了日志串口、TX/RX 是否接反或引脚配置是否和硬件一致。";
    }

    D_LOGW(TAG,
                "ML307C AT 超时: cmd=\"%s\", expect=\"%s\", 已收到 %u 字节，摘要=\"%s\"。可能原因: %s",
                clean_cmd,
                expect_text,
                (unsigned int)len,
                snippet,
                reason);
}

/**
 * @brief 循环发送 AT 检测，等待模块通信恢复。
 */
static int ml307c_wait_alive(struct ml307c_dev * dev, uint32_t timeout_ms)
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

/**
 * @brief 轮询蜂窝网络和链路状态直到联网成功。
 */
static int ml307c_wait_network_link(struct ml307c_dev * dev, uint32_t timeout_ms)
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
                D_LOGI(TAG, "ML307C 蜂窝数据网络已连接");
                return 0;
            }
        }

        dev->itf.delay_ms(ML307C_NET_POLL_MS);
    }

    dev->is_network_ok = 0u;
    D_LOGW(TAG, "ML307C 等待蜂窝数据网络连接超时");
    return -2;
}

/**
 * @brief 在 AT 响应中查找独立行前缀。
 */
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

/**
 * @brief 从指定响应前缀后解析第一个整数。
 */
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

/**
 * @brief 从指定响应前缀后提取连续数字字符串。
 */
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

/**
 * @brief 解析 CEREG 注册状态。
 */
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

/**
 * @brief 解析 DTUSTATE socket 通道状态。
 */
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

/**
 * @brief 初始化 ML307C 内部设备对象并绑定底层能力。
 */
static struct ml307c_dev * ml307c_init(ml307c_config_t *cfg, ml307c_interface_t *itf)
{
    if (cfg == NULL || itf == NULL) {
        D_LOGE(TAG, "ML307C 初始化失败: 配置或接口为空");
        return NULL;
    }
    if (itf->uart_write == NULL || itf->uart_read == NULL ||
        itf->delay_ms == NULL || itf->get_tick == NULL) {
        D_LOGE(TAG, "ML307C 初始化失败: 必要接口函数为空");
        return NULL;
    }

    struct ml307c_dev *dev = &s_ml307c_dev;
    memset(dev, 0, sizeof(*dev));
    memset(s_ml307c_rx_buf, 0, sizeof(s_ml307c_rx_buf));
    dev->config = *cfg;
    dev->itf = *itf;
    dev->rx_rb.buf = s_ml307c_rx_buf;
    dev->rx_rb.size = ML307C_RX_BUFFER_SIZE;

    D_LOGI(TAG, "ML307C RTU 驱动初始化成功, timeout=%u, socket_id=%u",
             (unsigned int)ml307c_get_timeout(dev),
             (unsigned int)ml307c_get_socket_id(dev));
    return dev;
}

/**
 * @brief 清理 ML307C 内部设备状态和接收缓存。
 */
static void ml307c_deinit(struct ml307c_dev * dev)
{
    if (dev == NULL) {
        return;
    }

    memset(s_ml307c_rx_buf, 0, sizeof(s_ml307c_rx_buf));
    memset(dev, 0, sizeof(*dev));
    D_LOGI(TAG, "ML307C 驱动已释放");
}

/**
 * @brief 发送 AT 命令检查模块是否可通信。
 */
static int ml307c_check_alive(struct ml307c_dev * dev)
{
    if (dev == NULL) {
        return -1;
    }

    uint32_t start = dev->itf.get_tick();
    uint32_t timeout_ms = ml307c_get_timeout(dev);
    uint32_t attempt_ms = timeout_ms < ML307C_ALIVE_ATTEMPT_MS ? timeout_ms : ML307C_ALIVE_ATTEMPT_MS;
    int ret = -3;

    do {
        ret = ml307c_send_cmd(dev, "AT" ML307C_CRLF, ML307C_OK, attempt_ms);
        if (ret == 0) {
            dev->is_ready = 1u;
            return 0;
        }

        dev->itf.delay_ms(ML307C_ALIVE_RETRY_GAP_MS);
    } while ((dev->itf.get_tick() - start) < timeout_ms);

    if (ret != 0) {
        dev->is_ready = 0u;
    }

    return ret;
}

/**
 * @brief 读取 ICCID 检查 SIM 卡是否可用。
 */
static int ml307c_check_sim(struct ml307c_dev * dev)
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

/**
 * @brief 查询并解析蜂窝网络注册状态。
 */
static int ml307c_get_network_state(struct ml307c_dev * dev, int *state)
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

/**
 * @brief 查询模块信号强度 CSQ。
 */
static int ml307c_get_signal(struct ml307c_dev * dev, int *rssi)
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

/**
 * @brief 查询 ML307C 数据链路状态。
 */
static int ml307c_get_link_state(struct ml307c_dev * dev, int *link)
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

/**
 * @brief 配置并等待 ML307C DTU socket 通道连接。
 */
static int ml307c_socket_connect(struct ml307c_dev * dev, const char *ip, int port, int proto)
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
        D_LOGW(TAG, "ML307C 复位命令未收到 OK，继续等待模块重启, ret=%d", ret);
    }

    dev->itf.delay_ms(ML307C_RESET_WAIT_MS);
    ret = ml307c_wait_alive(dev, ML307C_REBOOT_READY_MS);
    if (ret != 0) {
        D_LOGW(TAG, "ML307C 重启后 AT 未恢复, ret=%d", ret);
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
                D_LOGI(TAG, "ML307C socket 通道状态, id=%u, state=%d",
                         (unsigned int)socket_id, state);
                if (state == 1) {
                    D_LOGI(TAG, "ML307C socket 通道已连接, id=%u",
                             (unsigned int)socket_id);
                    return 0;
                }
            } else {
                D_LOGW(TAG, "ML307C DTUSTATE 响应解析失败");
            }
        }

        dev->itf.delay_ms(ML307C_SOCKET_POLL_MS);
    }

    D_LOGW(TAG, "ML307C socket 通道连接超时, id=%u", (unsigned int)socket_id);
    return -2;
}

/**
 * @brief 建立 ML307C TCP DTU 通道。
 */
static int ml307c_tcp_connect(struct ml307c_dev * dev, const char *ip, int port)
{
    return ml307c_socket_connect(dev, ip, port, 0);
}

/**
 * @brief 建立 ML307C UDP DTU 通道。
 */
static int ml307c_udp_connect(struct ml307c_dev * dev, const char *ip, int port)
{
    return ml307c_socket_connect(dev, ip, port, 1);
}

/**
 * @brief 通过指定 ML307C 路由发送原始数据。
 */
static int ml307c_send_route(struct ml307c_dev * dev, const char *route, uint8_t *data, int len)
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

    if (ml307c_uart_write_all(dev, (const uint8_t *)prefix, (uint32_t)prefix_len, ml307c_get_timeout(dev)) != 0) {
        return -3;
    }
    if (len > 0 && ml307c_uart_write_all(dev, data, (uint32_t)len, ml307c_get_timeout(dev)) != 0) {
        return -4;
    }
    if (ml307c_uart_write_all(dev, (const uint8_t *)ML307C_CRLF, 2u, ml307c_get_timeout(dev)) != 0) {
        return -5;
    }

    uint16_t downlink_len_before = s_ml307c_downlink_len;
    char resp[ML307C_LINE_MAX_LEN];
    int ret = ml307c_wait_response(dev,
                                   "AT+SENDR",
                                   "+SENDR",
                                   ml307c_get_timeout(dev),
                                   resp,
                                   sizeof(resp));
    if (ret != 0) {
        if (ret == -3 && s_ml307c_downlink_len != downlink_len_before) {
            D_LOGW(TAG, "ML307C SENDR 确认被下行数据抢占，按 UDP 已发出处理");
            return 0;
        }
        return ret;
    }

    int send_ret = -1;
    if (ml307c_parse_first_int_after(resp, "+SENDR", &send_ret) != 0) {
        return -6;
    }

    return send_ret == 0 ? 0 : -7;
}

/**
 * @brief 通过当前 TCP 路由发送数据。
 */
static int ml307c_tcp_send(struct ml307c_dev * dev, uint8_t *data, int len)
{
    uint8_t socket_id = ml307c_get_socket_id(dev);
    char route[8];
    snprintf(route, sizeof(route), "%u[1]", (unsigned int)socket_id);
    return ml307c_send_route(dev, route, data, len);
}

/**
 * @brief 通过当前 UDP 路由发送数据。
 */
static int ml307c_udp_send(struct ml307c_dev * dev, uint8_t *data, int len)
{
    uint8_t socket_id = ml307c_get_socket_id(dev);
    char route[8];
    snprintf(route, sizeof(route), "%u[1]", (unsigned int)socket_id);
    return ml307c_send_route(dev, route, data, len);
}

/**
 * @brief 从 ML307C UART 直接读取下行原始数据。
 */
static int ml307c_read_raw(struct ml307c_dev * dev, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (dev == NULL || buf == NULL || len == 0u) {
        return -1;
    }

    return dev->itf.uart_read(buf, len, timeout_ms);
}

/**
 * @brief 接收 HTTP 响应正文并剥离 ML307C HTTP 尾部状态。
 */
static int ml307c_http_capture_response(struct ml307c_dev * dev,
                                        uint8_t *resp,
                                        uint16_t resp_size,
                                        uint16_t *resp_len,
                                        uint32_t timeout_ms)
{
    uint32_t start = dev->itf.get_tick();
    uint16_t total = 0u;
    uint8_t trailer[256];
    uint16_t trailer_len = 0u;
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
            if (copy_len < (uint16_t)rlen) {
                uint16_t extra_len = (uint16_t)rlen - copy_len;
                if (extra_len > sizeof(trailer)) {
                    extra_len = sizeof(trailer);
                }
                if ((uint16_t)(trailer_len + extra_len) > sizeof(trailer)) {
                    uint16_t drop = (uint16_t)(trailer_len + extra_len - sizeof(trailer));
                    memmove(trailer, &trailer[drop], (size_t)(trailer_len - drop));
                    trailer_len = (uint16_t)(trailer_len - drop);
                }
                memcpy(&trailer[trailer_len], &tmp[copy_len], extra_len);
                trailer_len = (uint16_t)(trailer_len + extra_len);
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
            if (total >= resp_size &&
                ml307c_find_bytes(trailer, trailer_len, "+HTTP:") >= 0 &&
                ml307c_find_bytes(trailer, trailer_len, "\r\nOK") >= 0) {
                *resp_len = resp_size;
                return 0;
            }
        }

        dev->itf.delay_ms(10u);
    }

    *resp_len = total;
    if (total > 0u) {
        ml307c_log_timeout_summary("AT+HTTP response", "+HTTP/OK", resp, total);
    } else {
        D_LOGW(TAG, "ML307C HTTP 响应等待超时, received=0");
    }
    return -1;
}

/**
 * @brief 使用 ML307C HTTP AT 命令执行一次 POST。
 */
static int ml307c_http_post(struct ml307c_dev * dev,
                     uint8_t id,
                     const char *url,
                     const char *header,
                     const uint8_t *body,
                     uint16_t body_len,
                     uint8_t *resp,
                     uint16_t resp_size,
                     uint16_t *resp_len,
                     uint32_t timeout_ms)
{
    if (dev == NULL || url == NULL || (body_len > 0u && body == NULL) ||
        resp == NULL || resp_size == 0u || resp_len == NULL) {
        return -1;
    }

    if (id == 0u) {
        id = ML307C_HTTP_TASK_ID;
    }
    if (timeout_ms == 0u) {
        timeout_ms = ML307C_HTTP_TIMEOUT_MS;
    }

    const char *safe_header = header != NULL ? header : "";
    uint8_t method = body_len > 0u ? ML307C_HTTP_METHOD_POST : ML307C_HTTP_METHOD_GET;
    char cmd[768];

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPURL=%u,1,%u,\"%s\",\"%s\"" ML307C_CRLF,
             (unsigned int)id,
             (unsigned int)method,
             safe_header,
             url);
    int ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        D_LOGW(TAG, "ML307C HTTPURL 失败, ret=%d", ret);
        return ret;
    }

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTPCFG=%u,%u,%u,%u,%u" ML307C_CRLF,
             (unsigned int)id,
             (unsigned int)ML307C_HTTP_CONN_TIMEOUT_S,
             (unsigned int)ML307C_HTTP_RSP_TIMEOUT_S,
             (unsigned int)ML307C_HTTP_DNS_IPV4_FIRST,
             (unsigned int)ML307C_HTTP_ENCODE_NONE);
    ret = ml307c_send_cmd(dev, cmd, ML307C_OK, ml307c_get_timeout(dev));
    if (ret != 0) {
        D_LOGW(TAG, "ML307C HTTPCFG 不兼容或参数不支持, ret=%d, 继续使用默认 HTTP 配置", ret);
        if (ret != -2) {
            return ret;
        }
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
        D_LOGW(TAG, "ML307C HTTPRESP 失败, ret=%d", ret);
        return ret;
    }

    snprintf(cmd,
             sizeof(cmd),
             "AT+HTTP=%u,%u,%u,%u" ML307C_CRLF,
             (unsigned int)id,
             (unsigned int)body_len,
             (unsigned int)timeout_ms,
             (unsigned int)ML307C_HTTP_LATENCY_MS);

    ml307c_drain_uart(dev);
    ml307c_clear_buffer(dev);

    uint16_t cmd_len = (uint16_t)strlen(cmd);
    ret = ml307c_uart_write_all(dev, (const uint8_t *)cmd, cmd_len, ml307c_get_timeout(dev));
    if (ret != 0) {
        D_LOGW(TAG, "ML307C HTTP 命令写入失败, ret=%d, len=%u", ret, (unsigned int)cmd_len);
        return -2;
    }
    if (body_len > 0u) {
        ret = ml307c_wait_token(dev, "AT+HTTP", ">", ml307c_get_timeout(dev));
        if (ret != 0) {
            D_LOGW(TAG, "ML307C HTTP body 输入提示等待失败, ret=%d", ret);
            return -3;
        }
        ml307c_clear_buffer(dev);
        ret = ml307c_uart_write_all(dev, body, body_len, timeout_ms + 5000u);
        if (ret != 0) {
            D_LOGW(TAG, "ML307C HTTP body 写入失败, ret=%d, len=%u", ret, (unsigned int)body_len);
            return -4;
        }
    }

    *resp_len = 0u;
    ret = ml307c_http_capture_response(dev,
                                       resp,
                                       resp_size,
                                       resp_len,
                                       timeout_ms + 10000u);
    if (ret != 0) {
        D_LOGW(TAG,
               "ML307C HTTP 响应捕获失败, ret=%d, body_len=%u, resp_len=%u",
               ret,
               (unsigned int)body_len,
               (unsigned int)*resp_len);
    }
    return ret;
}

/**
 * @brief 对外执行 ML307C HTTP POST，并负责互斥保护和长度适配。
 */
int d_ml307c_http_post(const char *url,
                            const char *content_type,
                            const uint8_t *body,
                            uint32_t body_len,
                            uint8_t *resp,
                            uint32_t resp_size,
                            uint32_t *resp_len,
                            uint32_t timeout_ms)
{
    if (s_ml307c == NULL || url == NULL || resp == NULL || resp_len == NULL ||
        (body_len > 0u && body == NULL)) {
        return -1;
    }
    if (body_len > 65535u || resp_size > 65535u) {
        D_LOGW(TAG,
                    "ML307C HTTP 单次数据超限, body=%u, resp=%u",
                    (unsigned int)body_len,
                    (unsigned int)resp_size);
        return -2;
    }
    if (timeout_ms == 0u) {
        timeout_ms = ML307C_HTTP_TIMEOUT_MS;
    }

    char header[96];
    snprintf(header,
             sizeof(header),
             "Content-Type: %s",
             content_type != NULL ? content_type : "application/octet-stream");

    uint16_t local_resp_len = 0u;
    int ret = d_ml307c_lock(timeout_ms + 15000u);
    if (ret != 0) {
        return ret;
    }
    uint8_t http_id = d_ml307c_alloc_http_id();
    D_LOGI(TAG,
           "ML307C HTTP POST 开始, id=%u, body=%u, url=%s",
           (unsigned int)http_id,
           (unsigned int)body_len,
           url);
    ret = ml307c_http_post(s_ml307c,
                           http_id,
                           url,
                           header,
                           body_len > 0u ? body : (const uint8_t *)"",
                           (uint16_t)body_len,
                           resp,
                           (uint16_t)resp_size,
                           &local_resp_len,
                           timeout_ms);
    s_ml307c->itf.delay_ms(ML307C_HTTP_RECOVER_GAP_MS);
    d_ml307c_unlock();

    *resp_len = local_resp_len;
    if (ret != 0) {
        D_LOGW(TAG,
               "ML307C HTTP POST 失败, ret=%d, body=%u, resp_size=%u, resp_len=%u",
               ret,
               (unsigned int)body_len,
               (unsigned int)resp_size,
               (unsigned int)local_resp_len);
    }
    return ret;
}

/**
 * @brief 关闭当前 ML307C DTU socket 任务。
 */
static int ml307c_tcp_close(struct ml307c_dev * dev)
{
    if (dev == NULL) {
        return -1;
    }

    uint8_t socket_id = ml307c_get_socket_id(dev);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+DTUTASK=%u,0,\"SOCK\"" ML307C_CRLF, (unsigned int)socket_id);
    return ml307c_send_cmd(dev, cmd, "+DTUTASK", ml307c_get_timeout(dev));
}

/**
 * @brief 判断 CEREG 注册状态是否表示已注册。
 */
static int d_ml307c_reg_ready(int reg_state)
{
    return reg_state == 1 || reg_state == 5;
}

static void d_ml307c_cache_status(const d_ml307c_status_t *status)
{
    if (status == NULL) {
        return;
    }

    s_ml307c_cached_status = *status;
    s_ml307c_cached_status_valid = 1u;
    if (s_ml307c != NULL) {
        s_ml307c_cached_status_tick = s_ml307c->itf.get_tick();
    }
}

static void d_ml307c_fill_ready_status(d_ml307c_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->rssi = (s_ml307c_cached_status_valid != 0u &&
                    s_ml307c_cached_status.rssi >= 0 &&
                    s_ml307c_cached_status.rssi != 99)
                       ? s_ml307c_cached_status.rssi
                       : 20;
    status->reg_state = (s_ml307c_cached_status_valid != 0u &&
                         d_ml307c_reg_ready(s_ml307c_cached_status.reg_state))
                            ? s_ml307c_cached_status.reg_state
                            : 1;
    status->link_state = 1;
    status->sim_ready = 1;
    status->at_ready = 1;
}

/**
 * @brief 获取 ML307C 全局互斥锁。
 */
static int d_ml307c_lock(uint32_t timeout_ms)
{
    return s_ml307c_mutex != NULL ? osal_mutex_lock(s_ml307c_mutex, timeout_ms) : -1;
}

/**
 * @brief 释放 ML307C 全局互斥锁。
 */
static void d_ml307c_unlock(void)
{
    if (s_ml307c_mutex != NULL) {
        osal_mutex_unlock(s_ml307c_mutex);
    }
}

static uint8_t d_ml307c_alloc_http_id(void)
{
    uint8_t id = s_ml307c_http_next_id;
    if (id < ML307C_HTTP_TASK_ID || id > ML307C_HTTP_TASK_MAX_ID) {
        id = ML307C_HTTP_TASK_ID;
    }

    s_ml307c_http_next_id = (id >= ML307C_HTTP_TASK_MAX_ID)
                                ? ML307C_HTTP_TASK_ID
                                : (uint8_t)(id + 1u);
    return id;
}

/**
 * @brief 初始化当前板级 ML307C 网络驱动。
 */
int d_ml307c_init(const d_ml307c_wdriver_ops_t *ops, const ml307c_config_t *cfg)
{
    if (s_ml307c != NULL) {
        return 0;
    }
    if (ops == NULL ||
        ops->uart_write == NULL ||
        ops->uart_read == NULL ||
        ops->delay_ms == NULL ||
        ops->get_tick_ms == NULL) {
        return -1;
    }

    ml307c_config_t local_cfg = {
        .timeout_ms = D_ML307C_LOCK_TIMEOUT_MS,
        .socket_id = ML307C_DEFAULT_SOCKET_ID,
    };
    if (cfg != NULL) {
        local_cfg = *cfg;
    }

    ml307c_interface_t itf = {
        .uart_write = ops->uart_write,
        .uart_read = ops->uart_read,
        .delay_ms = ops->delay_ms,
        .get_tick = ops->get_tick_ms,
    };

    if (s_ml307c_mutex == NULL) {
        s_ml307c_mutex = osal_mutex_create();
        if (s_ml307c_mutex == NULL) {
            return -2;
        }
    }

    s_ml307c = ml307c_init(&local_cfg, &itf);
    if (s_ml307c == NULL) {
        return -3;
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        return ret;
    }

    D_LOGI(TAG, "ML307C 初始化步骤 1/2: 检测 AT 通信");
    ret = ml307c_check_alive(s_ml307c);
    if (ret == 0) {
        D_LOGI(TAG, "ML307C 初始化步骤 1/2: AT 通信正常");
        D_LOGI(TAG, "ML307C 初始化步骤 2/2: 检测 SIM 卡 ICCID");
        ret = ml307c_check_sim(s_ml307c);
        if (ret == 0) {
            D_LOGI(TAG, "ML307C 初始化步骤 2/2: SIM 卡正常");
        } else {
            D_LOGE(TAG, "ML307C 初始化失败: SIM 卡 ICCID 读取失败, ret=%d", ret);
        }
    } else {
        D_LOGE(TAG, "ML307C 初始化失败: AT 通信无有效响应, ret=%d", ret);
    }
    d_ml307c_unlock();

    if (ret != 0) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        s_ml307c_cached_status_valid = 0u;
        s_ml307c_cached_status_tick = 0u;
        return ret;
    }

    s_ml307c_tcp_connected = 0u;
    s_ml307c_udp_connected = 0u;
    s_ml307c_downlink_len = 0u;
    return 0;
}

int d_ml307c_prepare(const d_ml307c_wdriver_ops_t *ops, const ml307c_config_t *cfg)
{
    if (s_ml307c != NULL) {
        return 0;
    }
    if (ops == NULL ||
        ops->uart_write == NULL ||
        ops->uart_read == NULL ||
        ops->delay_ms == NULL ||
        ops->get_tick_ms == NULL) {
        return -1;
    }

    ml307c_config_t local_cfg = {
        .timeout_ms = D_ML307C_LOCK_TIMEOUT_MS,
        .socket_id = ML307C_DEFAULT_SOCKET_ID,
    };
    if (cfg != NULL) {
        local_cfg = *cfg;
    }

    ml307c_interface_t itf = {
        .uart_write = ops->uart_write,
        .uart_read = ops->uart_read,
        .delay_ms = ops->delay_ms,
        .get_tick = ops->get_tick_ms,
    };

    if (s_ml307c_mutex == NULL) {
        s_ml307c_mutex = osal_mutex_create();
        if (s_ml307c_mutex == NULL) {
            return -2;
        }
    }

    s_ml307c = ml307c_init(&local_cfg, &itf);
    if (s_ml307c == NULL) {
        return -3;
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        return ret;
    }

    ret = ml307c_check_alive(s_ml307c);
    if (ret != 0) {
        D_LOGE(TAG, "ML307C 准备失败: AT 通信无有效响应, ret=%d", ret);
        d_ml307c_unlock();
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
        s_ml307c_cached_status_valid = 0u;
        s_ml307c_cached_status_tick = 0u;
        return ret;
    }

    int sim_ret = ml307c_check_sim(s_ml307c);
    if (sim_ret != 0) {
        D_LOGW(TAG, "ML307C 准备完成但 SIM 不可用, ret=%d", sim_ret);
    }
    d_ml307c_unlock();

    s_ml307c_tcp_connected = 0u;
    s_ml307c_udp_connected = 0u;
    s_ml307c_downlink_len = 0u;
    return 0;
}

int d_ml307c_probe(d_ml307c_status_t *status)
{
    return d_ml307c_get_status(status);
}

/**
 * @brief 释放当前板级 ML307C 网络驱动。
 */
int d_ml307c_deinit(void)
{
    if (s_ml307c != NULL) {
        ml307c_deinit(s_ml307c);
        s_ml307c = NULL;
    }
    s_ml307c_tcp_connected = 0u;
    s_ml307c_udp_connected = 0u;
    s_ml307c_cached_status_valid = 0u;
    s_ml307c_cached_status_tick = 0u;
    s_ml307c_http_next_id = ML307C_HTTP_TASK_ID;
    s_ml307c_downlink_len = 0u;
    return 0;
}

/**
 * @brief 判断 ML307C 驱动是否已经初始化。
 */
int d_ml307c_is_initialized(void)
{
    return s_ml307c != NULL ? 1 : 0;
}

/**
 * @brief 获取 ML307C AT、SIM、信号、注册和链路状态。
 */
int d_ml307c_get_status(d_ml307c_status_t *status)
{
    if (s_ml307c == NULL || status == NULL) {
        return -1;
    }

    if (s_ml307c_cached_status_valid != 0u &&
        ((s_ml307c_udp_connected != 0u || s_ml307c_tcp_connected != 0u) ||
         (s_ml307c->itf.get_tick() - s_ml307c_cached_status_tick) < ML307C_STATUS_CACHE_MS)) {
        *status = s_ml307c_cached_status;
        return 0;
    }

    status->rssi = -1;
    status->reg_state = -1;
    status->link_state = -1;
    status->sim_ready = 0;
    status->at_ready = 0;

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }

    if (ml307c_check_alive(s_ml307c) == 0) {
        status->at_ready = 1;
    }
    if (ml307c_check_sim(s_ml307c) == 0) {
        status->sim_ready = 1;
    }
    (void)ml307c_get_signal(s_ml307c, &status->rssi);
    (void)ml307c_get_network_state(s_ml307c, &status->reg_state);
    (void)ml307c_get_link_state(s_ml307c, &status->link_state);

    d_ml307c_unlock();
    d_ml307c_cache_status(status);
    return 0;
}

/**
 * @brief 判断 ML307C 是否满足业务发送前的就绪条件。
 */
int d_ml307c_is_ready(void)
{
    d_ml307c_status_t status;
    int ret = d_ml307c_get_status(&status);
    if (ret != 0) {
        return ret;
    }

    return status.at_ready == 1 &&
           status.sim_ready == 1 &&
           d_ml307c_reg_ready(status.reg_state) &&
           status.link_state == 1;
}

/**
 * @brief 建立 ML307C TCP 通道。
 */
int d_ml307c_tcp_connect(const char *host, int port)
{
    if (s_ml307c == NULL || host == NULL || port <= 0) {
        return -1;
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    ret = ml307c_tcp_connect(s_ml307c, host, port);
    d_ml307c_unlock();
    if (ret == 0) {
        d_ml307c_status_t ready_status;
        d_ml307c_fill_ready_status(&ready_status);
        d_ml307c_cache_status(&ready_status);
        s_ml307c_tcp_connected = 1u;
    } else {
        s_ml307c_tcp_connected = 0u;
    }
    return ret;
}

/**
 * @brief 通过 ML307C TCP 通道发送数据。
 */
int d_ml307c_tcp_send(const uint8_t *data, int len)
{
    if (s_ml307c == NULL || data == NULL || len <= 0) {
        return -1;
    }
    if (s_ml307c_tcp_connected == 0u) {
        D_LOGW(TAG, "ML307C TCP 发送时通道未标记为已连接，仍尝试发送");
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    ret = ml307c_tcp_send(s_ml307c, (uint8_t *)data, len);
    d_ml307c_unlock();
    if (ret != 0) {
        s_ml307c_tcp_connected = 0u;
    }
    return ret;
}

/**
 * @brief 关闭 ML307C TCP 通道。
 */
int d_ml307c_tcp_close(void)
{
    if (s_ml307c == NULL) {
        return -1;
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    ret = ml307c_tcp_close(s_ml307c);
    d_ml307c_unlock();
    if (ret == 0) {
        s_ml307c_tcp_connected = 0u;
    }
    return ret;
}

/**
 * @brief 建立 ML307C UDP 通道。
 */
int d_ml307c_udp_connect(const char *host, int port)
{
    if (s_ml307c == NULL || host == NULL || port <= 0) {
        return -1;
    }
    if (s_ml307c_udp_connected != 0u) {
        return 0;
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    ret = ml307c_udp_connect(s_ml307c, host, port);
    d_ml307c_unlock();
    if (ret == 0) {
        d_ml307c_status_t ready_status;
        d_ml307c_fill_ready_status(&ready_status);
        d_ml307c_cache_status(&ready_status);
        s_ml307c_udp_connected = 1u;
    } else {
        s_ml307c_udp_connected = 0u;
    }
    return ret;
}

/**
 * @brief 通过 ML307C UDP 通道发送数据。
 */
int d_ml307c_udp_send(const uint8_t *data, int len)
{
    if (s_ml307c == NULL || data == NULL || len <= 0) {
        return -1;
    }
    if (s_ml307c_udp_connected == 0u) {
        D_LOGW(TAG, "ML307C UDP 发送时通道未标记为已连接，仍尝试发送");
    }

    int ret = d_ml307c_lock(D_ML307C_LOCK_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    ret = ml307c_udp_send(s_ml307c, (uint8_t *)data, len);
    d_ml307c_unlock();
    if (ret != 0) {
        s_ml307c_udp_connected = 0u;
    }
    return ret;
}

/**
 * @brief 读取 ML307C 下行原始数据。
 */
int d_ml307c_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (s_ml307c == NULL || buf == NULL || len == 0u) {
        return -1;
    }

    if (d_ml307c_lock(timeout_ms + 20u) != 0) {
        return 0;
    }
    int ret = ml307c_pop_downlink(buf, len);
    if (ret == 0) {
        ret = ml307c_read_raw(s_ml307c, buf, len, timeout_ms);
    }
    d_ml307c_unlock();
    return ret;
}
