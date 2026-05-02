/**
 * @file bsp_ml307c.h
 * @brief ML307C 4G 模块驱动公共接口。
 *
 * 本驱动通过 @ref ml307c_interface_t 注入 UART、延时和系统时基接口，
 * 便于在不同 BSP 或测试环境中复用。
 *
 * @version v1.0
 * @date 2026-04-28
 */
#ifndef ML307C_H
#define ML307C_H

#include <stdint.h>

/* ==================== 类型定义 ==================== */

/**
 * @brief ML307C 驱动依赖的硬件接口抽象。
 *
 * 用户需提供 UART 收发、毫秒延时和毫秒时基函数，驱动不直接依赖具体硬件实现。
 */
typedef struct {
    int (*uart_write)(uint8_t *data, uint16_t len);  /**< UART 发送函数。 */
    int (*uart_read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms); /**< UART 读取函数。 */
    void (*delay_ms)(uint32_t ms);                   /**< 毫秒延时函数。 */
    uint32_t (*get_tick)(void);                      /**< 获取系统毫秒 tick，用于超时计算。 */
} ml307c_interface_t;

/**
 * @brief ML307C 模块配置参数。
 */
typedef struct {
    uint32_t timeout_ms;   /**< AT 命令默认超时时间，单位为毫秒。 */
} ml307c_config_t;

/**
 * @brief 驱动内部使用的环形缓冲区。
 */
typedef struct {
    uint8_t *buf;   /**< 缓冲区内存首地址。 */
    uint16_t size;  /**< 缓冲区总大小，单位为字节。 */
    uint16_t head;  /**< 写入位置索引。 */
    uint16_t tail;  /**< 读取位置索引。 */
} ring_buffer_t;

/**
 * @brief ML307C 模块句柄。
 *
 * 句柄指向驱动内部对象，外部代码不应直接访问其成员。
 */
typedef struct ml307c_dev *ml307c_handle_t;

/* ==================== 公共函数 ==================== */

/**
 * @brief 初始化 ML307C 模块驱动实例。
 *
 * @param[in] cfg 配置参数。
 * @param[in] itf 接口函数指针集合。
 * @return 成功返回模块句柄，失败返回 NULL。
 * @note 调用此函数后应先调用 ml307c_check_alive() 确认模块正常。
 */
ml307c_handle_t ml307c_init(ml307c_config_t *cfg, ml307c_interface_t *itf);

/**
 * @brief 释放 ML307C 模块驱动实例。
 *
 * @param[in] dev 模块句柄。
 */
void ml307c_deinit(ml307c_handle_t dev);

/**
 * @brief 检查模块 AT 通信是否正常。
 *
 * @param[in] dev 模块句柄。
 * @return 成功返回 0；失败返回负值。
 * @note 发送 AT 命令并等待 OK 响应。
 */
int ml307c_check_alive(ml307c_handle_t dev);

/**
 * @brief       检查SIM卡是否正常
 * @param[in]   dev      模块句柄
 * @return      0成功（SIM卡正常），1无SIM卡或需要PIN/PUK，负值失败
 */
int ml307c_check_sim(ml307c_handle_t dev);

/**
 * @brief       检查网络注册状态
 * @param[in]   dev      模块句柄
 * @return      0已注册，1未注册，负值失败
 * @note        需要等待搜网完成（约10-30秒）
 */
int ml307c_check_network(ml307c_handle_t dev);

/**
 * @brief       获取信号强度
 * @param[in]   dev      模块句柄
 * @param[out]  rssi     信号强度输出（0-31，99=未知）
 * @return      0成功，负值失败
 * @note        rssi 0-31 表示信号强度，值越大越强；99表示未知
 */
int ml307c_get_signal(ml307c_handle_t dev, int *rssi);

/**
 * @brief       获取当前运营商名称
 * @param[in]   dev      模块句柄
 * @param[out]  buf      运营商名称输出缓冲区（建议至少16字节）
 * @return      0成功，负值失败
 */
int ml307c_get_operator(ml307c_handle_t dev, char *buf);

/**
 * @brief       打开数据网络
 * @param[in]   dev      模块句柄
 * @return      0成功，负值失败
 * @note        在进行TCP/UDP通信前需要先调用此函数
 */
int ml307c_open_net(ml307c_handle_t dev);

/**
 * @brief       关闭数据网络
 * @param[in]   dev      模块句柄
 * @return      0成功，负值失败
 */
int ml307c_close_net(ml307c_handle_t dev);

/**
 * @brief       建立TCP连接
 * @param[in]   dev      模块句柄
 * @param[in]   ip       目标IP地址或域名
 * @param[in]   port     目标端口号
 * @return      0成功，负值失败
 * @note        连接超时时间约为配置超时时间的2倍
 */
int ml307c_tcp_connect(ml307c_handle_t dev, const char *ip, int port);

/**
 * @brief       通过已建立的TCP连接发送数据
 * @param[in]   dev      模块句柄
 * @param[in]   data     数据缓冲区
 * @param[in]   len      数据长度
 * @return      0成功，负值失败
 */
int ml307c_tcp_send(ml307c_handle_t dev, uint8_t *data, int len);

/**
 * @brief       关闭TCP连接
 * @param[in]   dev      模块句柄
 * @return      0成功，负值失败
 */
int ml307c_tcp_close(ml307c_handle_t dev);

/**
 * @brief       发送短信
 * @param[in]   dev      模块句柄
 * @param[in]   num      目标手机号码
 * @param[in]   msg      短信内容（ASCII或GSM编码）
 * @return      0成功，负值失败
 * @note        使用TEXT模式发送
 */
int ml307c_send_sms(ml307c_handle_t dev, const char *num, const char *msg);

/**
 * @brief       获取模块IMEI
 * @param[in]   dev      模块句柄
 * @param[out]  buf      IMEI输出缓冲区（至少16字节）
 * @return      0成功，负值失败
 * @note        IMEI为15位数字
 */
int ml307c_get_imei(ml307c_handle_t dev, char *buf);

/**
 * @brief       获取SIM卡ICCID
 * @param[in]   dev      模块句柄
 * @param[out]  buf      ICCID输出缓冲区（至少21字节）
 * @return      0成功，负值失败
 * @note        ICCID为20位数字
 */
int ml307c_get_iccid(ml307c_handle_t dev, char *buf);

#endif /* ML307C_H */
