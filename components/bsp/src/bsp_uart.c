/**
 * @file bsp_uart.c
 * @brief BSP UART adapter implementation.
 */
#include "bsp_uart.h"

#include "bsp_config.h"
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
static const char *TAG = "bsp_uart";

/**
 * @brief 初始化 UART1 并安装 ESP-IDF UART 驱动。
 *
 * @return 成功返回 0；ESP-IDF API 失败时由 ESP_ERROR_CHECK 处理。
 */
int bsp_uart_init(void)
{
    if (s_uart_inited) {
        BSP_LOGI(TAG, "UART 已初始化");
        return 0;
    }

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(BSP_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BSP_UART_PORT, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uart_queue = osal_queue_create(10, sizeof(uint8_t));
    ESP_ERROR_CHECK(uart_driver_install(BSP_UART_PORT, 1024, 1024, 10,
                                       (QueueHandle_t *)uart_queue, 0));

    s_uart_inited = 1;
    BSP_LOGI(TAG, "UART 初始化成功, port=%d, baud=%d", BSP_UART_PORT, 115200);
    return 0;
}

/**
 * @brief 通过 UART1 发送数据。
 *
 * @param[in] data 待发送数据缓冲区。
 * @param[in] len 待发送数据长度，单位为字节。
 * @return 实际写入的字节数；失败返回负值。
 */
int bsp_uart_write(uint8_t *data, uint16_t len)
{
    int ret = uart_write_bytes(BSP_UART_PORT, (const char *)data, len);
    if (ret >= 0) {
        BSP_LOGI(TAG, "UART 发送完成, request=%u, written=%d",
                 (unsigned int)len, ret);
    } else {
        BSP_LOGE(TAG, "UART 发送失败, ret=%d", ret);
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
int bsp_uart_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    int read_len = uart_read_bytes(BSP_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
    if (read_len >= 0) {
        BSP_LOGI(TAG, "UART 接收完成, request=%u, read=%d",
                 (unsigned int)len, read_len);
        return read_len;
    }

    BSP_LOGE(TAG, "UART 接收失败, ret=%d", read_len);
    return 0;
}
