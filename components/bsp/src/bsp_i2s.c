/**
 * @file bsp_i2s.c
 * @brief 基于 ESP-IDF 新版 I2S standard 驱动的 BSP I2S 实现。
 */
#include "bsp_i2s.h"

#include "bsp_common.h"
#include "driver/i2s_std.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief I2S 日志标签。
 */
static const char *TAG = "bsp_i2s";

/**
 * @brief INMP441 麦克风 RX 通道句柄。
 */
static i2s_chan_handle_t s_i2s_rx_handle = NULL;

/**
 * @brief MAX98357A 功放 TX 通道句柄。
 */
static i2s_chan_handle_t s_i2s_tx_handle = NULL;

/**
 * @brief INMP441 麦克风使用的 I2S 控制器编号。
 */
#define BSP_I2S_INMP441_PORT I2S_NUM_0

/**
 * @brief MAX98357A 功放使用的 I2S 控制器编号。
 */
#define BSP_I2S_MAX98357A_PORT I2S_NUM_1

/**
 * @brief I2S 音频采样率。
 */
#define BSP_I2S_SAMPLE_RATE_HZ 16000u

/**
 * @brief I2S 读写默认超时时间，单位为毫秒。
 */
#define BSP_I2S_XFER_TIMEOUT_MS 1000u

/**
 * @brief 将底层驱动错误码转换为 BSP 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int bsp_i2s_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

/**
 * @brief 初始化 INMP441 使用的 I2S RX 通道。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2s_rx_init(void)
{
    if (s_i2s_rx_handle != NULL) {
        return 0;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_INMP441_PORT,
                                                            I2S_ROLE_MASTER);
    int ret = bsp_i2s_err_to_int(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_handle));
    if (ret != 0) {
        s_i2s_rx_handle = NULL;
        BSP_LOGE(TAG, "INMP441 I2S RX 通道创建失败, ret=%d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = BSP_I2S_SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_INMP441_BCLK_IO,
            .ws = BSP_INMP441_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = BSP_INMP441_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = bsp_i2s_err_to_int(i2s_channel_init_std_mode(s_i2s_rx_handle, &std_cfg));
    if (ret != 0) {
        i2s_del_channel(s_i2s_rx_handle);
        s_i2s_rx_handle = NULL;
        BSP_LOGE(TAG, "INMP441 I2S RX 标准模式配置失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_i2s_err_to_int(i2s_channel_enable(s_i2s_rx_handle));
    if (ret != 0) {
        i2s_del_channel(s_i2s_rx_handle);
        s_i2s_rx_handle = NULL;
        BSP_LOGE(TAG, "INMP441 I2S RX 通道使能失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "INMP441 I2S RX 初始化成功, sample_rate=%u",
             (unsigned int)BSP_I2S_SAMPLE_RATE_HZ);
    return 0;
}

/**
 * @brief 初始化 MAX98357A 使用的 I2S TX 通道。
 *
 * @return 成功返回 0；失败返回负值。
 */
static int bsp_i2s_tx_init(void)
{
    if (s_i2s_tx_handle != NULL) {
        return 0;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_MAX98357A_PORT,
                                                            I2S_ROLE_MASTER);
    int ret = bsp_i2s_err_to_int(i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, NULL));
    if (ret != 0) {
        s_i2s_tx_handle = NULL;
        BSP_LOGE(TAG, "MAX98357A I2S TX 通道创建失败, ret=%d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = BSP_I2S_SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_MAX98357A_BCLK_IO,
            .ws = BSP_MAX98357A_WS_IO,
            .dout = BSP_MAX98357A_DOUT_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = bsp_i2s_err_to_int(i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg));
    if (ret != 0) {
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
        BSP_LOGE(TAG, "MAX98357A I2S TX 标准模式配置失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_i2s_err_to_int(i2s_channel_enable(s_i2s_tx_handle));
    if (ret != 0) {
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
        BSP_LOGE(TAG, "MAX98357A I2S TX 通道使能失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "MAX98357A I2S TX 初始化成功, sample_rate=%u",
             (unsigned int)BSP_I2S_SAMPLE_RATE_HZ);
    return 0;
}

int bsp_i2s_init(void)
{
    int ret = bsp_i2s_rx_init();
    if (ret != 0) {
        BSP_LOGE(TAG, "BSP I2S 初始化失败: RX 初始化失败, ret=%d", ret);
        return ret;
    }

    ret = bsp_i2s_tx_init();
    if (ret != 0) {
        bsp_i2s_deinit();
        BSP_LOGE(TAG, "BSP I2S 初始化失败: TX 初始化失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "BSP I2S 初始化完成");
    return 0;
}

int bsp_i2s_deinit(void)
{
    int ret = 0;

    if (s_i2s_rx_handle != NULL) {
        int disable_ret = bsp_i2s_err_to_int(i2s_channel_disable(s_i2s_rx_handle));
        int del_ret = bsp_i2s_err_to_int(i2s_del_channel(s_i2s_rx_handle));
        s_i2s_rx_handle = NULL;
        BSP_LOGI(TAG, "INMP441 I2S RX 已释放, disable_ret=%d, del_ret=%d",
                 disable_ret, del_ret);
        if (ret == 0) {
            ret = (disable_ret != 0) ? disable_ret : del_ret;
        }
    }

    if (s_i2s_tx_handle != NULL) {
        int disable_ret = bsp_i2s_err_to_int(i2s_channel_disable(s_i2s_tx_handle));
        int del_ret = bsp_i2s_err_to_int(i2s_del_channel(s_i2s_tx_handle));
        s_i2s_tx_handle = NULL;
        BSP_LOGI(TAG, "MAX98357A I2S TX 已释放, disable_ret=%d, del_ret=%d",
                 disable_ret, del_ret);
        if (ret == 0) {
            ret = (disable_ret != 0) ? disable_ret : del_ret;
        }
    }

    return ret;
}

int inmp441_i2s_read_impl(uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if (s_i2s_rx_handle == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "INMP441 I2S 读取参数无效, handle=%p, data=%p, len=%u",
                 s_i2s_rx_handle, data, (unsigned int)len);
        return -1;
    }

    size_t bytes_read = 0;
    int ret = bsp_i2s_err_to_int(i2s_channel_read(s_i2s_rx_handle,
                                                  data,
                                                  len,
                                                  &bytes_read,
                                                  timeout_ms));
    if (ret == 0) {
        BSP_LOGI(TAG, "INMP441 I2S 读取成功, request=%u, read=%u",
                 (unsigned int)len, (unsigned int)bytes_read);
        return (int)bytes_read;
    }

    BSP_LOGE(TAG, "INMP441 I2S 读取失败, ret=%d", ret);
    return ret;
}

int max98357a_i2s_write_impl(const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if (s_i2s_tx_handle == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "MAX98357A I2S 写入参数无效, handle=%p, data=%p, len=%u",
                 s_i2s_tx_handle, data, (unsigned int)len);
        return -1;
    }

    size_t bytes_written = 0;
    int ret = bsp_i2s_err_to_int(i2s_channel_write(s_i2s_tx_handle,
                                                   data,
                                                   len,
                                                   &bytes_written,
                                                   timeout_ms));
    if (ret == 0) {
        BSP_LOGI(TAG, "MAX98357A I2S 写入成功, request=%u, written=%u",
                 (unsigned int)len, (unsigned int)bytes_written);
        return (int)bytes_written;
    }

    BSP_LOGE(TAG, "MAX98357A I2S 写入失败, ret=%d", ret);
    return ret;
}
