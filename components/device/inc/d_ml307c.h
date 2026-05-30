/**
 * @file d_ml307c.h
 * @brief ML307C 4G 模块驱动公共接口。
 *
 * 本驱动在初始化时注入 UART、延时和系统时基接口，对 service 层只暴露
 * 当前板级 ML307C 网络能力函数。
 *
 * @version v1.0
 * @date 2026-04-28
 */
#ifndef D_ML307C_H
#define D_ML307C_H

#include <stdint.h>

/**
 * @brief ML307C 驱动初始化所需的 WDRIVER 能力。
 */
typedef struct {
    int (*uart_write)(uint8_t *data, uint16_t len); /**< UART 发送函数。 */
    int (*uart_read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms); /**< UART 读取函数。 */
    void (*delay_ms)(uint32_t ms);                   /**< 毫秒延时函数。 */
    uint32_t (*get_tick_ms)(void);                   /**< 获取系统毫秒 tick。 */
} d_ml307c_wdriver_ops_t;

/**
 * @brief ML307C 模块配置参数。
 */
typedef struct {
    uint32_t timeout_ms;   /**< AT 命令默认超时时间，单位为毫秒。 */
    uint8_t socket_id;     /**< DTU socket 通道号，范围 1 到 4；填 0 时默认使用 1。 */
} ml307c_config_t;

/**
 * @brief ML307C 网络状态快照。
 */
typedef struct {
    int rssi;       /**< CSQ 信号值，0-31 有效，99 未知。 */
    int reg_state;  /**< CEREG 注册状态。 */
    int link_state; /**< ISLINK 链路状态。 */
    int sim_ready;  /**< SIM 是否正常。 */
    int at_ready;   /**< AT 通信是否正常。 */
} d_ml307c_status_t;

/**
 * @brief 初始化当前板级 ML307C 网络驱动。
 *
 * @param[in] ops WDRIVER 能力函数表。
 * @param[in] cfg ML307C 配置。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_init(const d_ml307c_wdriver_ops_t *ops, const ml307c_config_t *cfg);

/**
 * @brief 准备 ML307C 驱动资源并做轻量 AT/SIM 检查。
 *
 * 当前实现复用 d_ml307c_init()，但语义上供 app 层非阻塞启动编排使用。
 *
 * @param[in] ops WDRIVER 能力函数表。
 * @param[in] cfg ML307C 配置。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_prepare(const d_ml307c_wdriver_ops_t *ops, const ml307c_config_t *cfg);

/**
 * @brief 探测 ML307C 当前可用状态。
 *
 * @param[out] status 状态输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_probe(d_ml307c_status_t *status);

/**
 * @brief 释放当前板级 ML307C 网络驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_deinit(void);

/**
 * @brief 判断 ML307C 驱动是否已经初始化。
 *
 * @return 已初始化返回 1；否则返回 0。
 */
int d_ml307c_is_initialized(void);

/**
 * @brief 获取 ML307C 当前网络状态。
 *
 * @param[out] status 状态输出地址。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_get_status(d_ml307c_status_t *status);

/**
 * @brief 判断 ML307C 是否已满足业务发送条件。
 *
 * @return 已就绪返回 1；未就绪返回 0；查询失败返回负值。
 */
int d_ml307c_is_ready(void);

/**
 * @brief 建立 ML307C TCP 通道。
 *
 * @param[in] host 目标主机名或 IP。
 * @param[in] port 目标端口。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_tcp_connect(const char *host, int port);

/**
 * @brief 通过 ML307C TCP 通道发送数据。
 *
 * @param[in] data 待发送数据。
 * @param[in] len 数据长度。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_tcp_send(const uint8_t *data, int len);

/**
 * @brief 关闭 ML307C TCP 通道。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_tcp_close(void);

/**
 * @brief 建立 ML307C UDP 通道。
 *
 * @param[in] host 目标主机名或 IP。
 * @param[in] port 目标端口。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_udp_connect(const char *host, int port);

/**
 * @brief 通过 ML307C UDP 通道发送数据。
 *
 * @param[in] data 待发送数据。
 * @param[in] len 数据长度。
 * @return 成功返回 0；失败返回负值。
 */
int d_ml307c_udp_send(const uint8_t *data, int len);

/**
 * @brief 读取 ML307C 下行原始数据。
 *
 * @param[out] buf 接收缓冲区。
 * @param[in] len 缓冲区长度。
 * @param[in] timeout_ms 读取超时时间，单位毫秒。
 * @return 实际读取字节数；失败返回负值。
 */
int d_ml307c_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief 通过 ML307C 执行一次 HTTP POST。
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
int d_ml307c_http_post(const char *url,
                            const char *content_type,
                            const uint8_t *body,
                            uint32_t body_len,
                            uint8_t *resp,
                            uint32_t resp_size,
                            uint32_t *resp_len,
                            uint32_t timeout_ms);

#endif /* D_ML307C_H */
