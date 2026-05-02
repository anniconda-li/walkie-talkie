/**
 * @file bsp_uart.h
 * @brief BSP UART 底层驱动抽象层。
 *
 * 提供 ML307C 模块使用的 UART 初始化、发送和接收接口，供上层模块驱动注入使用。
 */
#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>
#include "driver/uart.h"

/**
 * @brief ML307C 模块连接的 UART 控制器编号。
 */
#define UART_PORT_ML307C UART_NUM_1

/**
 * @brief 初始化 ML307C 使用的 UART 外设。
 *
 * @return 成功返回 0；失败时由 ESP_ERROR_CHECK 触发错误处理。
 */
int bsp_uart_init(void);

/**
 * @brief ML307C UART 发送适配函数。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败时返回负值。
 */
int ml307c_uart_write_impl(uint8_t *data, uint16_t len);

/**
 * @brief ML307C UART 接收适配函数。
 *
 * @param[out] buf 接收数据缓冲区。
 * @param[in] len 期望读取的最大长度，单位为字节。
 * @param[in] timeout_ms 接收超时时间，单位为毫秒。
 * @return 实际读取的字节数；超时或失败返回 0。
 */
int ml307c_uart_read_impl(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

#endif
