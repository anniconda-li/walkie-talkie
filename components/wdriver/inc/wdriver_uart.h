/**
 * @file wdriver_uart.h
 * @brief WDRIVER UART 适配接口。
 */
#ifndef WDRIVER_UART_H
#define WDRIVER_UART_H

#include <stdint.h>
#include "driver/uart.h"

/**
 * @brief 板级 UART 控制器端口号，用于外部串口设备。
 */
#define WDRIVER_UART_PORT UART_NUM_1

/**
 * @brief 板级 UART 默认波特率，用于外部串口设备。
 */
#define WDRIVER_UART_BAUD_RATE 115200

/**
 * @brief 初始化板级 UART 外设。
 *
 * @return 成功返回 0；失败时由 ESP_ERROR_CHECK 触发错误处理。
 */
int wdriver_uart_init(void);

/**
 * @brief 向板级 UART 写入数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败时返回负值。
 */
int wdriver_uart_write(uint8_t *data, uint16_t len);

/**
 * @brief 从板级 UART 读取数据。
 *
 * @param[out] buf 接收数据缓冲区。
 * @param[in] len 期望读取的最大长度，单位为字节。
 * @param[in] timeout_ms 接收超时时间，单位为毫秒。
 * @return 实际读取的字节数；超时或失败返回 0。
 */
int wdriver_uart_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

#endif
