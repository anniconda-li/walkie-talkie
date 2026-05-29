/**
 * @file service_network.h
 * @brief 网络能力服务接口。
 *
 * 本服务对上提供网络状态查询、TCP/UDP 和 HTTP POST 能力。底层网络链路
 * 通过能力接口绑定，app 层只需要面向网络能力编排业务。
 */
#ifndef SERVICE_NETWORK_H
#define SERVICE_NETWORK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通用网络状态快照。
 */
typedef struct {
    int rssi;          /**< 信号强度，0-31 表示有效，99 表示未知，负值表示查询失败。 */
    int reg_state;     /**< CEREG 注册状态：1 本地注册，5 漫游注册。 */
    int link_state;    /**< ISLINK 数据链路状态：1 已连接，0 未连接。 */
    int sim_ready;     /**< 蜂窝 SIM 或等效链路前置条件：1 正常，0 异常。 */
    int at_ready;      /**< 蜂窝 AT 或等效驱动通信状态：1 正常，0 异常。 */
} service_network_status_t;

/**
 * @brief 网络服务依赖的下层网络能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层网络 driver 是否已初始化。 */
    int (*get_status)(service_network_status_t *status); /**< 获取网络状态。 */
    int (*is_ready)(void);                            /**< 判断网络是否就绪。 */
    int (*tcp_connect)(const char *host, int port);   /**< 建立 TCP 连接。 */
    int (*tcp_send)(const uint8_t *data, int len);    /**< 发送 TCP 数据。 */
    int (*tcp_close)(void);                           /**< 关闭 TCP 连接。 */
    int (*udp_connect)(const char *host, int port);   /**< 建立或配置 UDP 通道。 */
    int (*udp_send)(const uint8_t *data, int len);    /**< 发送 UDP 数据。 */
    int (*read_downlink)(uint8_t *buf, uint16_t len, uint32_t timeout_ms); /**< 读取下行数据。 */
    int (*http_post)(const char *url,
                     const char *content_type,
                     const uint8_t *body,
                     uint32_t body_len,
                     uint8_t *resp,
                     uint32_t resp_size,
                     uint32_t *resp_len,
                     uint32_t timeout_ms); /**< HTTP POST 二进制数据并读取响应。 */
} service_network_ops_t;

/**
 * @brief 初始化网络服务。
 *
 * 初始化时会复制 ops 中的网络能力函数表，并检查下层 driver 是否已经初始化。
 * 后续所有网络 API 都通过 service 内部保存的 ops 调用。
 *
 * @param[in] ops 网络能力函数表；为 NULL 时仅检查 service 是否已经初始化。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_init(const service_network_ops_t *ops);

/**
 * @brief 释放网络服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_network_deinit(void);

/**
 * @brief 获取当前网络状态快照。
 *
 * @param[out] status 状态输出。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_get_status(service_network_status_t *status);

/**
 * @brief 判断网络是否已经可以进行数据业务。
 *
 * @return 已就绪返回 1；未就绪返回 0；查询失败返回负值。
 */
int service_network_is_ready(void);

/**
 * @brief 配置并等待 TCP 通道连接。
 *
 * @param[in] host 服务器 IP 或域名。
 * @param[in] port 服务器端口。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_tcp_connect(const char *host, int port);

/**
 * @brief 通过 TCP 通道发送数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位字节。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_tcp_send(const uint8_t *data, int len);

/**
 * @brief 关闭当前 TCP 通道。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_network_tcp_close(void);

/**
 * @brief 配置并等待 UDP DTU 通道就绪。
 *
 * 对 ML307C 后端表示配置 DTU UDP 通道；对 WiFi 后端表示记录 UDP 目标
 * 并准备 socket 收发。
 *
 * @param[in] host 服务器 IP 或域名。
 * @param[in] port 服务器端口。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_udp_connect(const char *host, int port);

/**
 * @brief 通过 UDP DTU 通道发送数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位字节。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_udp_send(const uint8_t *data, int len);

/**
 * @brief 读取网络下行数据。
 *
 * @param[out] buf 输出缓冲区。
 * @param[in] len 最大读取字节数。
 * @param[in] timeout_ms 读取超时，单位毫秒。
 * @return 读取字节数；超时返回 0；失败返回负值。
 */
int service_network_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief HTTP POST 上传二进制数据，并返回响应 body。
 *
 * App 层的 AI 分片协议使用该接口上传 WAV 分片、查询状态和拉取回复分片。
 * 具体网络后端负责处理 WiFi HTTP client 或 ML307C AT+HTTP 差异。
 *
 * @param[in] url 请求 URL。
 * @param[in] content_type 请求 Content-Type；为 NULL 时由后端使用默认值。
 * @param[in] body 请求体缓冲区；body_len 为 0 时允许为 NULL。
 * @param[in] body_len 请求体长度。
 * @param[out] resp 响应 body 输出缓冲区。
 * @param[in] resp_size 响应缓冲区长度。
 * @param[out] resp_len 实际响应长度。
 * @param[in] timeout_ms 请求等待响应的超时时间，单位毫秒。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_http_post(const char *url,
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

#endif /* SERVICE_NETWORK_H */
