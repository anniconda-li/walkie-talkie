/**
 * @file driver_ml307c.h
 * @brief ML307C 4G 模块驱动公共接口。
 *
 * 本驱动在初始化时注入 UART、延时和系统时基接口，对 service 层只暴露
 * 当前板级 ML307C 网络能力函数。
 *
 * @version v1.0
 * @date 2026-04-28
 */
#ifndef ML307C_H
#define ML307C_H

#include <stdint.h>

/**
 * @brief ML307C 驱动初始化所需的 BSP 能力。
 */
typedef struct {
    int (*uart_write)(uint8_t *data, uint16_t len); /**< UART 发送函数。 */
    int (*uart_read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms); /**< UART 读取函数。 */
    void (*delay_ms)(uint32_t ms);                   /**< 毫秒延时函数。 */
    uint32_t (*get_tick_ms)(void);                   /**< 获取系统毫秒 tick。 */
} driver_ml307c_bsp_ops_t;

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
} driver_ml307c_status_t;

/**
 * @brief 初始化当前板级 ML307C 网络驱动。
 *
 * @param[in] ops BSP 能力函数表。
 * @param[in] cfg ML307C 配置。
 * @return 成功返回 0；失败返回负值。
 */
int driver_ml307c_init(const driver_ml307c_bsp_ops_t *ops, const ml307c_config_t *cfg);

/**
 * @brief 释放当前板级 ML307C 网络驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_ml307c_deinit(void);

int driver_ml307c_is_initialized(void);
int driver_ml307c_get_status(driver_ml307c_status_t *status);
int driver_ml307c_is_ready(void);
int driver_ml307c_tcp_connect(const char *host, int port);
int driver_ml307c_tcp_send(const uint8_t *data, int len);
int driver_ml307c_tcp_close(void);
int driver_ml307c_udp_connect(const char *host, int port);
int driver_ml307c_udp_send(const uint8_t *data, int len);
int driver_ml307c_read_downlink(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
int driver_ml307c_http_post(const char *url,
                            const char *content_type,
                            const uint8_t *body,
                            uint32_t body_len,
                            uint8_t *resp,
                            uint32_t resp_size,
                            uint32_t *resp_len,
                            uint32_t timeout_ms);
int driver_ml307c_http_post_wav(const char *url,
                                const uint8_t *wav,
                                uint16_t wav_len,
                                uint8_t *resp,
                                uint16_t resp_size,
                                uint16_t *resp_len,
                                uint32_t timeout_ms);

#endif /* ML307C_H */
