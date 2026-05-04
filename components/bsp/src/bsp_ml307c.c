/**
 * @file bsp_ml307c.c
 * @brief ML307C 4G 模块驱动实现。
 */

#include "bsp_ml307c.h"

#include "bsp_common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==================== 常量定义 ==================== */
#define CRLF          "\r\n"
#define CMD_OK        "OK"
#define CMD_ERROR     "ERROR"
#define CMD_CREG      "+CREG:"
#define CMD_CSQ       "+CSQ:"
#define CMD_ICCID     "+ICCID:"
#define CMD_SMOK      "SMS OK"
#define LINE_MAX_LEN  256

/**
 * @brief ML307C 日志标签。
 */
static const char *TAG = "bsp_ml307c";

/* ==================== 内部结构体 ==================== */
struct ml307c_dev {
    ml307c_config_t   config;        /**< 配置参数 */
    ml307c_interface_t *itf;         /**< 接口函数指针 */
    ring_buffer_t      rx_rb;         /**< 接收环形缓冲区 */
    uint8_t           is_ready;      /**< 模块就绪标志 */
    uint8_t           is_network_ok; /**< 网络就绪标志 */
};

/* ==================== 内部函数声明 ==================== */

/**
 * @brief 发送 AT 命令并等待期望响应。
 *
 * @param[in] dev 模块句柄。
 * @param[in] cmd AT 命令字符串。
 * @param[in] expect 期望响应字符串，可为 NULL 或空字符串。
 * @param[in] timeout 超时时间，单位为毫秒。
 * @return 成功返回 0；失败返回负值。
 * @note 函数会在发送命令前清空接收缓冲区。
 */
static int ml307c_send_cmd(
    ml307c_handle_t dev,
    const char *cmd,
    const char *expect,
    uint32_t timeout);

/**
 * @brief 从 UART 接收流中读取一行数据。
 *
 * @param[in] dev 模块句柄。
 * @param[out] line 行数据输出缓冲区。
 * @param[in] line_size 行数据输出缓冲区大小。
 * @return 成功返回 0；无完整数据返回 -1。
 * @note 以 \r 或 \n 为行分隔符，并跳过空行。
 */
static int ml307c_read_line(ml307c_handle_t dev, char *line, uint16_t line_size);

/**
 * @brief 在环形缓冲区中搜索子串。
 *
 * @param[in] dev 模块句柄。
 * @param[in] expect 要搜索的字符串。
 * @return 找到返回 0；未找到返回 -1。
 */
static int ml307c_search_in_buffer(ml307c_handle_t dev, const char *expect);

/**
 * @brief       等待期望的响应字符串
 * @param[in]   dev      模块句柄
 * @param[in]   expect   期望的响应字符串
 * @param[in]   timeout  超时时间（毫秒）
 * @return      0成功，-1超时
 * @note        持续读取UART数据并存入环形缓冲区直到找到期望字符串或超时
 */
static int ml307c_wait_response(
    ml307c_handle_t dev,
    const char *expect,
    uint32_t timeout);

/**
 * @brief       解析CSQ响应获取信号强度
 * @param[in]   buf      AT响应行，格式: +CSQ: <rssi>,<ber>
 * @param[out]  rssi     信号强度输出（0-31，99=未知）
 * @return      0成功，负值失败
 */
static int ml307c_parse_csq(const char *buf, int *rssi);

/**
 * @brief       解析CREG响应获取网络注册状态
 * @param[in]   buf      AT响应行，格式: +CREG: <n>,<stat>
 * @return      注册状态值（0-5），负值失败
 * @note        0=未注册，1=已注册本地，5=已注册漫游
 */
static int ml307c_parse_creg(const char *buf);

/**
 * @brief       清空接收环形缓冲区
 * @param[in]   dev      模块句柄
 */
static void ml307c_clear_buffer(ml307c_handle_t dev);

/**
 * @brief       向环形缓冲区追加数据
 * @param[in]   dev      模块句柄
 * @param[in]   data     数据指针
 * @param[in]   len      数据长度
 * @note        缓冲区满时自动覆盖旧数据
 */
static void ml307c_append_rx(ml307c_handle_t dev, uint8_t *data, int len);

/* ==================== 公共函数实现 ==================== */

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
    dev->itf    = itf;
    dev->is_ready      = 0;
    dev->is_network_ok = 0;

    dev->rx_rb.buf  = (uint8_t *)calloc(1, 1024);
    if (dev->rx_rb.buf == NULL) {
        free(dev);
        BSP_LOGE(TAG, "ML307C 初始化失败: 接收缓冲区内存分配失败");
        return NULL;
    }
    dev->rx_rb.size = 1024;
    dev->rx_rb.head = 0;
    dev->rx_rb.tail = 0;

    BSP_LOGI(TAG, "ML307C 驱动初始化成功, timeout=%u, rx_buf=%u",
             (unsigned int)dev->config.timeout_ms,
             (unsigned int)dev->rx_rb.size);
    return dev;
}

void ml307c_deinit(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    if (dev->rx_rb.buf != NULL) {
        free(dev->rx_rb.buf);
        dev->rx_rb.buf = NULL;
    }

    free(dev);
    BSP_LOGI(TAG, "ML307C 驱动已释放");
}

int ml307c_check_alive(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }
    return ml307c_send_cmd(dev, "AT" CRLF, CMD_OK, dev->config.timeout_ms);
}

int ml307c_check_sim(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    int ret = ml307c_send_cmd(dev, "AT+CPIN?" CRLF, "+CPIN: READY", dev->config.timeout_ms);
    if (ret == 0) {
        return 0;
    }

    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+CPIN?" CRLF, "", 100);
    char line[LINE_MAX_LEN];
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strstr(line, "SIM PIN") || strstr(line, "SIM PUK") ||
            strstr(line, "NOT INSERTED")) {
            return 1;
        }
    }

    return ret;
}

int ml307c_check_network(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    ml307c_clear_buffer(dev);
    int ret = ml307c_send_cmd(dev, "AT+CREG?" CRLF, CMD_CREG, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    char line[LINE_MAX_LEN];
    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+CREG?" CRLF, "", 100);
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strstr(line, CMD_CREG)) {
            int stat = ml307c_parse_creg(line);
            if (stat == 1 || stat == 5) {
                dev->is_network_ok = 1;
                return 0;
            }
            return 1;
        }
    }

    return -2;
}

int ml307c_get_signal(ml307c_handle_t dev, int *rssi)
{
    if (dev == NULL || rssi == NULL) {
        return -1;
    }

    ml307c_clear_buffer(dev);
    int ret = ml307c_send_cmd(dev, "AT+CSQ" CRLF, CMD_CSQ, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    char line[LINE_MAX_LEN];
    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+CSQ" CRLF, "", 100);
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strstr(line, CMD_CSQ)) {
            return ml307c_parse_csq(line, rssi);
        }
    }

    return -2;
}

int ml307c_get_operator(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    ml307c_clear_buffer(dev);
    int ret = ml307c_send_cmd(dev, "AT+COPS?" CRLF, "+COPS:", dev->config.timeout_ms);
    if (ret != 0) {
        ml307c_send_cmd(dev, "AT+COPS=0" CRLF, CMD_OK, dev->config.timeout_ms);
        dev->itf->delay_ms(5000);
        ret = ml307c_send_cmd(dev, "AT+COPS?" CRLF, "+COPS:", dev->config.timeout_ms);
        if (ret != 0) {
            return ret;
        }
    }

    char line[LINE_MAX_LEN];
    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+COPS?" CRLF, "", 100);
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strstr(line, "+COPS:")) {
            char *p = strchr(line, '"');
            if (p) {
                p++;
                char *q = strchr(p, '"');
                if (q) {
                    int len = (q - p) < 16 ? (q - p) : 16;
                    strncpy(buf, p, len);
                    buf[len] = '\0';
                    return 0;
                }
            }
        }
    }

    return -2;
}

int ml307c_open_net(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    int ret = ml307c_send_cmd(dev, "AT+CGREG=1" CRLF, CMD_OK, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    dev->itf->delay_ms(2000);

    return 0;
}

int ml307c_close_net(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    int ret = ml307c_send_cmd(dev, "AT+CGREG=0" CRLF, CMD_OK, dev->config.timeout_ms);
    if (ret == 0) {
        dev->is_network_ok = 0;
    }
    return ret;
}

int ml307c_tcp_connect(ml307c_handle_t dev, const char *ip, int port)
{
    if (dev == NULL || ip == NULL || port <= 0) {
        return -1;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+TCPCONN=%s,%d" CRLF, ip, port);

    return ml307c_send_cmd(dev, cmd, "CONNECT OK", dev->config.timeout_ms * 2);
}

int ml307c_tcp_send(ml307c_handle_t dev, uint8_t *data, int len)
{
    if (dev == NULL || data == NULL || len <= 0) {
        return -1;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+TCPSEND=%d" CRLF, len);

    int ret = ml307c_send_cmd(dev, cmd, ">", dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    dev->itf->uart_write(data, len);

    return ml307c_wait_response(dev, "SEND OK", dev->config.timeout_ms);
}

int ml307c_tcp_close(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return -1;
    }

    return ml307c_send_cmd(dev, "AT+TCPCLOSE" CRLF, CMD_OK, dev->config.timeout_ms);
}

int ml307c_send_sms(ml307c_handle_t dev, const char *num, const char *msg)
{
    if (dev == NULL || num == NULL || msg == NULL) {
        return -1;
    }

    int ret = ml307c_send_cmd(dev, "AT+CMGF=1" CRLF, CMD_OK, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"" CRLF, num);

    ret = ml307c_send_cmd(dev, cmd, ">", dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    uint8_t buf[256];
    int len = snprintf((char *)buf, sizeof(buf), "%s", msg);
    buf[len++] = 0x1A;  /* Ctrl+Z */

    dev->itf->uart_write(buf, len);

    return ml307c_wait_response(dev, CMD_SMOK, dev->config.timeout_ms * 2);
}

int ml307c_get_imei(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    ml307c_clear_buffer(dev);
    int ret = ml307c_send_cmd(dev, "AT+GSN" CRLF, CMD_OK, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    char line[LINE_MAX_LEN];
    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+GSN" CRLF, "", 100);
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strlen(line) >= 15) {
            int i;
            for (i = 0; i < 15 && line[i] >= '0' && line[i] <= '9'; i++) {
                buf[i] = line[i];
            }
            if (i == 15) {
                buf[15] = '\0';
                return 0;
            }
        }
    }

    return -2;
}

int ml307c_get_iccid(ml307c_handle_t dev, char *buf)
{
    if (dev == NULL || buf == NULL) {
        return -1;
    }

    ml307c_clear_buffer(dev);
    int ret = ml307c_send_cmd(dev, "AT+ICCID" CRLF, CMD_ICCID, dev->config.timeout_ms);
    if (ret != 0) {
        return ret;
    }

    char line[LINE_MAX_LEN];
    ml307c_clear_buffer(dev);
    ml307c_send_cmd(dev, "AT+ICCID" CRLF, "", 100);
    while (ml307c_read_line(dev, line, sizeof(line)) == 0) {
        if (strstr(line, CMD_ICCID)) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                int i = 0;
                while (*p >= '0' && *p <= '9' && i < 20) {
                    buf[i++] = *p++;
                }
                buf[i] = '\0';
                return 0;
            }
        }
    }

    return -2;
}

/* ==================== 内部函数实现 ==================== */

static int ml307c_send_cmd(
    ml307c_handle_t dev,
    const char *cmd,
    const char *expect,
    uint32_t timeout)
{
    if (dev == NULL || cmd == NULL) {
        BSP_LOGE(TAG, "ML307C 发送命令参数无效, dev=%p, cmd=%p", dev, cmd);
        return -1;
    }

    ml307c_clear_buffer(dev);

    uint16_t len = (uint16_t)strlen(cmd);
    BSP_LOGI(TAG, "ML307C 发送 AT 命令, len=%u, expect=%s",
             (unsigned int)len,
             (expect != NULL && expect[0] != '\0') ? expect : "none");
    int write_len = dev->itf->uart_write((uint8_t *)cmd, len);
    if (write_len < 0) {
        BSP_LOGE(TAG, "ML307C AT 命令发送失败, ret=%d", write_len);
        return write_len;
    }

    if (expect == NULL || expect[0] == '\0') {
        BSP_LOGI(TAG, "ML307C AT 命令无需等待响应");
        return 0;
    }

    return ml307c_wait_response(dev, expect, timeout);
}

static int ml307c_read_line(ml307c_handle_t dev, char *line, uint16_t line_size)
{
    if (dev == NULL || line == NULL || line_size == 0) {
        return -1;
    }

    int idx = 0;
    line[0] = '\0';

    ring_buffer_t *rb = &dev->rx_rb;

    while (idx < line_size - 1) {
        if (rb->head == rb->tail) {
            uint8_t tmp[64];
            int rlen = dev->itf->uart_read(tmp, sizeof(tmp), 100);
            if (rlen > 0) {
                ml307c_append_rx(dev, tmp, rlen);
            } else {
                break;
            }
        }

        uint8_t c = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % rb->size;

        if (c == '\r' || c == '\n') {
            if (idx > 0) {
                break;
            }
            continue;
        }

        line[idx++] = (char)c;
    }

    line[idx] = '\0';

    if (idx > 0) {
        return 0;
    }

    return -1;
}
/**
 * @brief       在环形缓冲区中搜索子串（将环形缓冲区复制为线性数组）
 * @param[in]   dev      模块句柄
 * @param[in]   expect   要搜索的字符串
 * @return      0找到，-1未找到
 */
static int ml307c_search_in_buffer(ml307c_handle_t dev, const char *expect)
{
    ring_buffer_t *rb = &dev->rx_rb;
    int expect_len = strlen(expect);

    /* 计算缓冲区中实际数据长度 */
    int data_len;
    if (rb->head >= rb->tail) {
        data_len = rb->head - rb->tail;
    } else {
        data_len = (rb->size - rb->tail) + rb->head;
    }

    if (data_len < expect_len) {
        return -1;
    }

    /* 复制到临时线性缓冲区 */
    static char linear_buf[512];
    int idx = 0;

    if (rb->head >= rb->tail) {
        memcpy(linear_buf, &rb->buf[rb->tail], rb->head - rb->tail);
        idx = rb->head - rb->tail;
    } else {
        int len1 = rb->size - rb->tail;
        memcpy(linear_buf, &rb->buf[rb->tail], len1);
        memcpy(linear_buf + len1, rb->buf, rb->head);
        idx = len1 + rb->head;
    }
    linear_buf[idx] = '\0';

    /* 在线性缓冲区中搜索 */
    if (strstr(linear_buf, expect) != NULL) {
        return 0;
    }

    return -1;
}

/**
 * @brief       等待期望的响应字符串
 * @param[in]   dev      模块句柄
 * @param[in]   expect   期望的响应字符串
 * @param[in]   timeout  超时时间（毫秒）
 * @return      0成功，-1超时
 * @note        持续读取UART数据并存入环形缓冲区直到找到期望字符串或超时
 */
static int ml307c_wait_response(
    ml307c_handle_t dev,
    const char *expect,
    uint32_t timeout)
{
    if (dev == NULL || expect == NULL) {
        BSP_LOGE(TAG, "ML307C 等待响应参数无效, dev=%p, expect=%p", dev, expect);
        return -1;
    }

    uint32_t start = dev->itf->get_tick();

    while ((dev->itf->get_tick() - start) < timeout) {
        uint8_t tmp[64];
        int rlen = dev->itf->uart_read(tmp, sizeof(tmp), 100);
        if (rlen > 0) {
            ml307c_append_rx(dev, tmp, rlen);
        }

        /* 在缓冲区中搜索期望字符串 */
        if (ml307c_search_in_buffer(dev, expect) == 0) {
            BSP_LOGI(TAG, "ML307C 收到期望响应: %s", expect);
            return 0;  /* 找到期望字符串 */
        }

        dev->itf->delay_ms(10);
    }

    BSP_LOGW(TAG, "ML307C 等待响应超时: %s", expect);
    return -1;  /* 超时 */
}

static int ml307c_parse_csq(const char *buf, int *rssi)
{
    if (buf == NULL || rssi == NULL) {
        return -1;
    }

    const char *p = strchr(buf, ':');
    if (p == NULL) {
        return -2;
    }
    p++;

    while (*p == ' ' || *p == '\t') p++;

    int val = atoi(p);
    if (val < 0 || val > 31) {
        *rssi = 99;
    } else {
        *rssi = val;
    }

    return 0;
}

static int ml307c_parse_creg(const char *buf)
{
    if (buf == NULL) {
        return -1;
    }

    const char *p = strchr(buf, ':');
    if (p == NULL) {
        return -2;
    }
    p++;

    p = strchr(p, ',');
    if (p == NULL) {
        return -3;
    }
    p++;

    return atoi(p);
}

static void ml307c_clear_buffer(ml307c_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    ring_buffer_t *rb = &dev->rx_rb;
    rb->head = 0;
    rb->tail = 0;

    if (rb->buf) {
        memset(rb->buf, 0, rb->size);
    }
}

static void ml307c_append_rx(ml307c_handle_t dev, uint8_t *data, int len)
{
    if (dev == NULL || data == NULL || len <= 0) {
        return;
    }

    ring_buffer_t *rb = &dev->rx_rb;

    for (int i = 0; i < len; i++) {
        rb->buf[rb->head] = data[i];
        rb->head = (rb->head + 1) % rb->size;

        if (rb->head == rb->tail) {
            rb->tail = (rb->tail + 1) % rb->size;
        }
    }
}
