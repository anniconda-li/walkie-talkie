/**
 * @file bsp_uart.c
 * @brief ML307C 使用的 UART BSP 适配实现。
 */
#include "bsp_uart.h"

#include "driver/uart.h"
#include "osal_queue.h"
#include "freertos/FreeRTOS.h"

#include <stdlib.h>
#include <string.h>

static osal_queue_t uart_queue;

/**
 * @brief 初始化 UART1 并安装 ESP-IDF UART 驱动。
 *
 * @return 成功返回 0；ESP-IDF API 失败时由 ESP_ERROR_CHECK 处理。
 */
int bsp_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT_ML307C, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_ML307C, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uart_queue = osal_queue_create(10, sizeof(uint8_t));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_ML307C, 1024, 1024, 10,
                                       (QueueHandle_t *)uart_queue, 0));

    return 0;
}

/**
 * @brief 通过 UART1 向 ML307C 发送数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败返回负值。
 */
int ml307c_uart_write_impl(uint8_t *data, uint16_t len)
{
    return uart_write_bytes(UART_PORT_ML307C, (const char *)data, len);
}

/**
 * @brief 从 UART1 读取 ML307C 返回的数据。
 *
 * @param[out] buf 接收缓冲区。
 * @param[in] len 最大读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；超时或失败返回 0。
 */
int ml307c_uart_read_impl(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    int read_len = uart_read_bytes(UART_PORT_ML307C, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (read_len >= 0) ? read_len : 0;
}
