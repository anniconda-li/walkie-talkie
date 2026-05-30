/**
 * @file d_wifi.h
 * @brief WiFi STA 网络驱动接口。
 */
#ifndef D_WIFI_H
#define D_WIFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi STA 初始化配置。
 */
typedef struct {
    const char *ssid;     /**< WiFi SSID。 */
    const char *password; /**< WiFi 密码。 */
} d_wifi_config_t;

/**
 * @brief WiFi 网络状态。
 */
typedef struct {
    int rssi;       /**< RSSI 映射后的 0-31 信号值；99 表示未知。 */
    int link_ready; /**< STA 获取 IP 后为 1，否则为 0。 */
    char ssid[33];  /**< 当前连接的 SSID，未连接时为空。 */
} d_wifi_status_t;

/**
 * @brief WiFi 扫描结果。
 */
typedef struct {
    char ssid[33];   /**< SSID，保证以 \0 结尾。 */
    int rssi;        /**< RSSI dBm。 */
    int authmode;    /**< esp_wifi authmode 原始值。 */
} d_wifi_ap_record_t;

/**
 * @brief 准备 WiFi STA 内部资源，不等待联网。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_prepare(void);

/**
 * @brief 扫描附近 WiFi 热点。
 *
 * @param[out] records 结果数组。
 * @param[in] max_records 数组容量。
 * @param[out] count 实际结果数量。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_scan(d_wifi_ap_record_t *records, uint16_t max_records, uint16_t *count);

/**
 * @brief 连接指定 WiFi 并等待获取 IP。
 *
 * @param[in] ssid SSID。
 * @param[in] password 密码，可为空字符串。
 * @param[in] timeout_ms 等待获取 IP 的最长时间。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief 初始化 WiFi STA 并等待联网完成。
 *
 * @param[in] cfg WiFi 初始化配置。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_init(const d_wifi_config_t *cfg);

/**
 * @brief 释放 WiFi 网络层资源。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_deinit(void);

/**
 * @brief 判断 WiFi STA 是否已经启动。
 *
 * @return 已启动返回 1；否则返回 0。
 */
int d_wifi_is_initialized(void);

/**
 * @brief 获取 WiFi 当前网络状态。
 *
 * @param[out] status 状态输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_get_status(d_wifi_status_t *status);

/**
 * @brief 判断 WiFi 是否已经获取 IP。
 *
 * @return 已就绪返回 1；否则返回 0。
 */
int d_wifi_is_ready(void);

/**
 * @brief 创建 UDP socket 并设置目标地址。
 *
 * @param[in] host 目标主机名或 IPv4 地址。
 * @param[in] port 目标端口。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_udp_connect(const char *host, int port);

/**
 * @brief 通过 UDP 发送数据。
 *
 * @param[in] data 待发送数据。
 * @param[in] len 数据长度。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_udp_send(const uint8_t *data, int len);

/**
 * @brief 读取 UDP 下行数据。
 *
 * @param[out] buf 接收缓冲区。
 * @param[in] len 缓冲区长度。
 * @param[in] timeout_ms 读取超时时间，单位毫秒。
 * @return 实际读取字节数；无数据返回 0；失败返回负值。
 */
int d_wifi_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief 建立 TCP 连接。
 *
 * @param[in] host 目标主机名或 IPv4 地址。
 * @param[in] port 目标端口。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_tcp_connect(const char *host, int port);

/**
 * @brief 通过 TCP 发送数据。
 *
 * @param[in] data 待发送数据。
 * @param[in] len 数据长度。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_tcp_send(const uint8_t *data, int len);

/**
 * @brief 关闭 TCP 连接。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_tcp_close(void);

/**
 * @brief 执行一次 HTTP POST。
 *
 * @param[in] url 请求 URL。
 * @param[in] content_type 请求体 Content-Type，NULL 时使用 application/octet-stream。
 * @param[in] body 请求体数据。
 * @param[in] body_len 请求体长度。
 * @param[out] resp 响应缓冲区。
 * @param[in] resp_size 响应缓冲区长度。
 * @param[out] resp_len 实际响应长度。
 * @param[in] timeout_ms 超时时间，单位毫秒；0 使用默认值。
 * @return 成功返回 0；失败返回负值。
 */
int d_wifi_http_post(const char *url,
                          const char *content_type,
                          const uint8_t *body,
                          uint32_t body_len,
                          uint8_t *resp,
                          uint32_t resp_size,
                          uint32_t *resp_len,
                          uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* D_WIFI_H */
