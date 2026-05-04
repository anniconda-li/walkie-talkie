/**
 * @file bsp_spi.c
 * @brief BSP SPI 主机总线驱动实现。
 */
#include "bsp_spi.h"

#include "bsp_common.h"

#include <stdint.h>

/**
 * @brief SPI 日志标签。
 */
static const char *TAG = "bsp_spi";

/**
 * @brief 项目默认 SPI host。
 */
#define BSP_SPI_HOST SPI2_HOST

/**
 * @brief 当前 SPI 最大传输大小。
 *
 * 当前按 ST7789 LCD 全屏 RGB565 数据大小配置；后续增加 SD 卡时不影响该值。
 */
#define BSP_SPI_MAX_TRANSFER_SZ (320u * 240u * sizeof(uint16_t))

/**
 * @brief SPI 总线是否已经初始化。
 */
static int s_spi_bus_inited = 0;

/**
 * @brief 将底层驱动错误码转换为 BSP 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int bsp_spi_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

int bsp_spi_init(void)
{
    if (s_spi_bus_inited) {
        BSP_LOGI(TAG, "SPI 总线已初始化");
        return 0;
    }

    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = BSP_LCD_MOSI_IO,
        .miso_io_num = BSP_LCD_MISO_IO,
        .sclk_io_num = BSP_LCD_SCK_IO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = (int)BSP_SPI_MAX_TRANSFER_SZ,
    };

    int ret = bsp_spi_err_to_int(spi_bus_initialize(BSP_SPI_HOST,
                                                    &spi_bus_cfg,
                                                    SPI_DMA_CH_AUTO));
    if (ret != 0) {
        BSP_LOGE(TAG, "SPI 总线初始化失败, ret=%d", ret);
        return ret;
    }

    s_spi_bus_inited = 1;
    BSP_LOGI(TAG, "SPI 总线初始化成功, host=%d, sck=%d, mosi=%d, miso=%d",
             BSP_SPI_HOST, BSP_LCD_SCK_IO, BSP_LCD_MOSI_IO, BSP_LCD_MISO_IO);
    return 0;
}

int bsp_spi_deinit(void)
{
    if (!s_spi_bus_inited) {
        BSP_LOGI(TAG, "SPI 总线未初始化，无需释放");
        return 0;
    }

    int ret = bsp_spi_err_to_int(spi_bus_free(BSP_SPI_HOST));
    if (ret == 0) {
        s_spi_bus_inited = 0;
        BSP_LOGI(TAG, "SPI 总线释放成功");
    } else {
        BSP_LOGE(TAG, "SPI 总线释放失败, ret=%d", ret);
    }

    return ret;
}

spi_host_device_t bsp_spi_get_host(void)
{
    return BSP_SPI_HOST;
}
