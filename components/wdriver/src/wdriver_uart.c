/**
 * @file wdriver_uart.c
 * @brief WDRIVER UART adapter implementation.
 */
#include "wdriver_uart.h"

#include "wdriver_config.h"
#include "driver/uart.h"
#include "osal_queue.h"
#include "freertos/FreeRTOS.h"

#include <stdlib.h>
#include <string.h>

static osal_queue_t uart_queue;

/**
 * @brief UART 是否已经初始化。
 */
static int s_uart_inited = 0;

/**
 * @brief UART 日志标签。
 */
static const char *TAG = "wdriver_uart";

/**
 * @brief 初始化 UART1 并安装 ESP-IDF UART 驱动。
 *
 * @return 成功返回 0；ESP-IDF API 失败时由 ESP_ERROR_CHECK 处理。
 */
int wdriver_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = WDRIVER_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(WDRIVER_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(WDRIVER_UART_PORT,
                                 WDRIVER_UART_TX_IO,
                                 WDRIVER_UART_RX_IO,
                                 WDRIVER_UART_RTS_IO,
                                 WDRIVER_UART_CTS_IO));

    if (s_uart_inited) {
        WDRIVER_LOGI(TAG, "UART 已初始化，已刷新参数和引脚, port=%d, baud=%d",
                     WDRIVER_UART_PORT,
                     WDRIVER_UART_BAUD_RATE);
        return 0;
    }

    uart_queue = osal_queue_create(10, sizeof(uint8_t));
    ESP_ERROR_CHECK(uart_driver_install(WDRIVER_UART_PORT, 1024, 1024, 10,
                                       (QueueHandle_t *)uart_queue, 0));

    s_uart_inited = 1;
    WDRIVER_LOGI(TAG, "UART 初始化成功, port=%d, baud=%d", WDRIVER_UART_PORT, WDRIVER_UART_BAUD_RATE);
    return 0;
}

/**
 * @brief 通过 UART1 发送数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败返回负值。
 */
int wdriver_uart_write(uint8_t *data, uint16_t len)
{
    int ret = uart_write_bytes(WDRIVER_UART_PORT, (const char *)data, len);
    if (ret < 0) {
        WDRIVER_LOGE(TAG, "UART 发送失败, ret=%d", ret);
    }

    return ret;
}

/**
 * @brief 从 UART1 读取数据。
 *
 * @param[out] buf 接收缓冲区。
 * @param[in] len 最大读取长度，单位为字节。
 * @param[in] timeout_ms 读取超时时间，单位为毫秒。
 * @return 实际读取字节数；超时或失败返回 0。
 */
int wdriver_uart_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    int read_len = uart_read_bytes(WDRIVER_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
    if (read_len >= 0) {
        return read_len;
    }

    WDRIVER_LOGE(TAG, "UART 接收失败, ret=%d", read_len);
    return 0;
}
