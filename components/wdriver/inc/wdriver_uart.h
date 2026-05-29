/**
 * @file wdriver_uart.h
 * @brief WDRIVER UART adapter.
 */
#ifndef WDRIVER_UART_H
#define WDRIVER_UART_H

#include <stdint.h>
#include "driver/uart.h"

/**
 * @brief Board UART controller used by external serial devices.
 */
#define WDRIVER_UART_PORT UART_NUM_1

/**
 * @brief Initialize the board UART peripheral.
 *
 * @return 成功返回 0；失败时由 ESP_ERROR_CHECK 触发错误处理。
 */
int wdriver_uart_init(void);

/**
 * @brief Write bytes to the board UART.
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败时返回负值。
 */
int wdriver_uart_write(uint8_t *data, uint16_t len);

/**
 * @brief Read bytes from the board UART.
 *
 * @param[out] buf 接收数据缓冲区。
 * @param[in] len 期望读取的最大长度，单位为字节。
 * @param[in] timeout_ms 接收超时时间，单位为毫秒。
 * @return 实际读取的字节数；超时或失败返回 0。
 */
int wdriver_uart_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

#endif
