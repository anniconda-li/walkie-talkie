/**
 * @file service_network.h
 * @brief 网络能力服务接口。
 *
 * 本服务对上提供蜂窝网络状态查询和 TCP 发送能力，隐藏 ML307C 模块、
 * UART 注入和 AT 指令细节。app 层只需要面向网络能力编排业务。
 */
#ifndef SERVICE_NETWORK_H
#define SERVICE_NETWORK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 网络服务初始化配置。
 */
typedef struct {
    uint32_t at_timeout_ms; /**< AT 命令默认超时时间，单位毫秒；填 0 使用默认值。 */
    uint8_t socket_id;      /**< DTU socket 通道号；填 0 使用默认通道 1。 */
} service_network_config_t;

/**
 * @brief 蜂窝网络状态快照。
 */
typedef struct {
    int rssi;          /**< 信号强度，0-31 表示有效，99 表示未知，负值表示查询失败。 */
    int reg_state;     /**< CEREG 注册状态：1 本地注册，5 漫游注册。 */
    int link_state;    /**< ISLINK 数据链路状态：1 已连接，0 未连接。 */
    int sim_ready;     /**< SIM 卡状态：1 正常，0 异常。 */
    int at_ready;      /**< AT 通信状态：1 正常，0 异常。 */
} service_network_status_t;

/**
 * @brief 初始化网络服务。
 *
 * @param[in] cfg 初始化配置，可为 NULL。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_init(const service_network_config_t *cfg);

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
 * @brief 读取 ML307C 下行透传数据。
 *
 * @param[out] buf 输出缓冲区。
 * @param[in] len 最大读取字节数。
 * @param[in] timeout_ms 读取超时，单位毫秒。
 * @return 读取字节数；超时返回 0；失败返回负值。
 */
int service_network_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief HTTP POST 上传 WAV，并返回响应 body。
 *
 * @param[in] url 请求 URL。
 * @param[in] wav WAV 数据缓冲区。
 * @param[in] wav_len WAV 数据长度。
 * @param[out] resp 响应 body 输出缓冲区。
 * @param[in] resp_size 响应缓冲区长度。
 * @param[out] resp_len 实际响应长度。
 * @return 成功返回 0；失败返回负值。
 */
int service_network_http_post_wav(const char *url,
                                  const uint8_t *wav,
                                  uint16_t wav_len,
                                  uint8_t *resp,
                                  uint16_t resp_size,
                                  uint16_t *resp_len);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_NETWORK_H */
