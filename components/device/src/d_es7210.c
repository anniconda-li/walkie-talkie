/**
 * @file d_es7210.c
 * @brief ES7210 四通道音频 ADC 驱动实现。
 */
#include "d_es7210.h"

#include "d_config.h"
#include "osal_task.h"

#include <string.h>

static const char *TAG = "d_es7210";

#define ES7210_RESET_REG00          0x00u /**< 复位和芯片状态控制寄存器。 */
#define ES7210_MAINCLK_REG02        0x02u /**< 主时钟配置寄存器。 */
#define ES7210_LRCK_DIVH_REG04      0x04u /**< LRCK 分频高字节寄存器。 */
#define ES7210_LRCK_DIVL_REG05      0x05u /**< LRCK 分频低字节寄存器。 */
#define ES7210_POWER_DOWN_REG06     0x06u /**< ADC 数字电源控制寄存器。 */
#define ES7210_OSR_REG07            0x07u /**< ADC 过采样率配置寄存器。 */
#define ES7210_TIME_CONTROL0_REG09  0x09u /**< ADC 时序控制寄存器 0。 */
#define ES7210_TIME_CONTROL1_REG0A  0x0Au /**< ADC 时序控制寄存器 1。 */
#define ES7210_SDP_INTERFACE1_REG11 0x11u /**< 串行音频接口格式寄存器 1。 */
#define ES7210_SDP_INTERFACE2_REG12 0x12u /**< 串行音频接口格式寄存器 2。 */
#define ES7210_ADC1_VOLUME_REG1B    0x1Bu /**< ADC1 数字音量寄存器。 */
#define ES7210_ADC2_VOLUME_REG1C    0x1Cu /**< ADC2 数字音量寄存器。 */
#define ES7210_ADC3_VOLUME_REG1D    0x1Du /**< ADC3 数字音量寄存器。 */
#define ES7210_ADC4_VOLUME_REG1E    0x1Eu /**< ADC4 数字音量寄存器。 */
#define ES7210_ADC34_HPF2_REG20     0x20u /**< ADC3/4 高通滤波配置寄存器 2。 */
#define ES7210_ADC34_HPF1_REG21     0x21u /**< ADC3/4 高通滤波配置寄存器 1。 */
#define ES7210_ADC12_HPF2_REG22     0x22u /**< ADC1/2 高通滤波配置寄存器 2。 */
#define ES7210_ADC12_HPF1_REG23     0x23u /**< ADC1/2 高通滤波配置寄存器 1。 */
#define ES7210_ANALOG_REG40         0x40u /**< 模拟前端全局配置寄存器。 */
#define ES7210_MIC12_BIAS_REG41     0x41u /**< MIC1/2 偏置配置寄存器。 */
#define ES7210_MIC34_BIAS_REG42     0x42u /**< MIC3/4 偏置配置寄存器。 */
#define ES7210_MIC1_GAIN_REG43      0x43u /**< MIC1 模拟增益寄存器。 */
#define ES7210_MIC2_GAIN_REG44      0x44u /**< MIC2 模拟增益寄存器。 */
#define ES7210_MIC3_GAIN_REG45      0x45u /**< MIC3 模拟增益寄存器。 */
#define ES7210_MIC4_GAIN_REG46      0x46u /**< MIC4 模拟增益寄存器。 */
#define ES7210_MIC1_POWER_REG47     0x47u /**< MIC1 输入电源控制寄存器。 */
#define ES7210_MIC2_POWER_REG48     0x48u /**< MIC2 输入电源控制寄存器。 */
#define ES7210_MIC3_POWER_REG49     0x49u /**< MIC3 输入电源控制寄存器。 */
#define ES7210_MIC4_POWER_REG4A     0x4Au /**< MIC4 输入电源控制寄存器。 */
#define ES7210_MIC12_POWER_REG4B    0x4Bu /**< MIC1/2 通道电源控制寄存器。 */
#define ES7210_MIC34_POWER_REG4C    0x4Cu /**< MIC3/4 通道电源控制寄存器。 */

/** @brief ES7210 驱动是否已完成初始化。 */
static uint8_t s_es7210_inited = 0u;

/** @brief 初始化时承接并保存的 WDRIVER 能力函数表。 */
static d_es7210_wdriver_ops_t s_d_ops;

/** @brief 读取日志节流计数器，避免高频 PCM 读取刷屏。 */
static uint32_t s_d_read_log_count = 0u;

/** @brief 通道峰值日志节流计数器。 */
static uint32_t s_d_channel_log_count = 0u;

/** @brief ES7210 原始双通道 PCM 读取缓存。 */
static uint8_t s_d_raw_buf[D_ES7210_MAX_FRAMES * 4u];

#define D_ES7210_CAPTURE_MIC1   1
#define D_ES7210_CAPTURE_MIC2   2
#define D_ES7210_CAPTURE_MIX12  3

/**
 * @brief 当前回环测试默认只输出 MIC2。
 *
 * 如需单独验证某一路，可临时改为 D_ES7210_CAPTURE_MIC1 或
 * D_ES7210_CAPTURE_MIX12。
 */
#ifndef D_ES7210_CAPTURE_SELECT
#define D_ES7210_CAPTURE_SELECT D_ES7210_CAPTURE_MIX12
#endif

/**
 * @brief 向 ES7210 写入一个 8 位寄存器值。
 */
static int es7210_write_u8(uint8_t reg, uint8_t value)
{
    return s_d_ops.i2c_write_reg(D_ES7210_I2C_ADDR,
                                      D_ES7210_I2C_SPEED_HZ,
                                      reg,
                                      &value,
                                      1u);
}

/**
 * @brief 写入 ES7210 默认初始化寄存器序列。
 */
static int es7210_config_default(void)
{
    static const struct {
        uint8_t reg;
        uint8_t value;
        uint16_t delay_ms;
    } init_seq[] = {
        /* 复位芯片并等待内部状态稳定。 */
        {ES7210_RESET_REG00, 0xFF},
        {ES7210_RESET_REG00, 0x32, 20u},

        /* 配置采样时序、高通滤波和 I2S 数据格式。 */
        {ES7210_TIME_CONTROL0_REG09, 0x30},
        {ES7210_TIME_CONTROL1_REG0A, 0x30},
        {ES7210_ADC12_HPF1_REG23, 0x2A},
        {ES7210_ADC12_HPF2_REG22, 0x0A},
        {ES7210_ADC34_HPF1_REG21, 0x2A},
        {ES7210_ADC34_HPF2_REG20, 0x0A},
        {ES7210_SDP_INTERFACE1_REG11, 0x60}, /* I2S，16 bit。 */
        {ES7210_SDP_INTERFACE2_REG12, 0x00}, /* 标准 I2S，关闭 TDM。 */

        /* 配置模拟前端、MIC 偏置、通道增益和通道电源。 */
        {ES7210_ANALOG_REG40, 0xC3},
        {ES7210_MIC12_BIAS_REG41, 0x70},
        {ES7210_MIC34_BIAS_REG42, 0x70},
        {ES7210_MIC1_GAIN_REG43, 0x1F},
        {ES7210_MIC2_GAIN_REG44, 0x1F},
        {ES7210_MIC3_GAIN_REG45, 0x10},
        {ES7210_MIC4_GAIN_REG46, 0x10},
        {ES7210_MIC1_POWER_REG47, 0x08},
        {ES7210_MIC2_POWER_REG48, 0x08},
        {ES7210_MIC3_POWER_REG49, 0xFF},
        {ES7210_MIC4_POWER_REG4A, 0xFF},

        /* 配置 16 kHz 采样所需的 OSR、MCLK 和 LRCK 分频。 */
        {ES7210_OSR_REG07, 0x20},
        {ES7210_MAINCLK_REG02, 0xC1, 20u}, /* 16 kHz，MCLK=4.096 MHz。 */
        {ES7210_LRCK_DIVH_REG04, 0x01},
        {ES7210_LRCK_DIVL_REG05, 0x00},

        /* 打开 ADC 通路并设置四路 ADC 音量。 */
        {ES7210_POWER_DOWN_REG06, 0x04},
        {ES7210_MIC12_POWER_REG4B, 0x0F},
        {ES7210_MIC34_POWER_REG4C, 0x00},
        {ES7210_ADC1_VOLUME_REG1B, 0xBF},
        {ES7210_ADC2_VOLUME_REG1C, 0xBF},
        {ES7210_ADC3_VOLUME_REG1D, 0xBF},
        {ES7210_ADC4_VOLUME_REG1E, 0xBF},

        /* 退出复位，开始正常采集。 */
        {ES7210_RESET_REG00, 0x71},
        {ES7210_RESET_REG00, 0x41, 20u},
    };

    for (unsigned int i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        int ret = es7210_write_u8(init_seq[i].reg, init_seq[i].value);
        if (ret != 0) {
            D_LOGE(TAG, "ES7210 寄存器配置失败, reg=0x%02X, ret=%d",
                     (unsigned int)init_seq[i].reg, ret);
            return ret;
        }
        if (init_seq[i].delay_ms > 0u) {
            osal_delay_ms(init_seq[i].delay_ms);
        }
    }

    D_LOGI(TAG, "ES7210 芯片配置完成, sample_rate=16000, bits=16");
    return 0;
}

/**
 * @brief 读取一段 ES7210 PCM 原始数据并做日志节流。
 */
static int es7210_read(uint8_t *data,
                       uint32_t len,
                       uint32_t timeout_ms)
{
    if (s_es7210_inited == 0u || data == NULL || len == 0) {
        D_LOGE(TAG, "ES7210 读取参数无效, inited=%u, data=%p, len=%u",
                 (unsigned int)s_es7210_inited, data, (unsigned int)len);
        return -1;
    }

    int ret = s_d_ops.i2s_read(data, len, timeout_ms);
    if (ret >= 0) {
        s_d_read_log_count++;
        if ((s_d_read_log_count % 100u) != 0u) {
            return ret;
        }
        D_LOGI(TAG, "ES7210 读取完成, request=%u, read=%d",
                 (unsigned int)len, ret);
    } else {
        D_LOGE(TAG, "ES7210 读取失败, ret=%d", ret);
    }

    return ret;
}

/**
 * @brief 从小端 PCM 字节流中取出一个 16 位样本。
 */
static int16_t d_es7210_read_i16_le(const uint8_t *data, uint32_t sample_index)
{
    uint32_t offset = sample_index * 2u;
    uint16_t raw = (uint16_t)data[offset] | ((uint16_t)data[offset + 1u] << 8);
    return (int16_t)raw;
}

static int d_es7210_abs_i16(int16_t sample)
{
    int value = sample;
    return value < 0 ? -value : value;
}

int d_es7210_init(const d_es7210_wdriver_ops_t *ops)
{
    if (s_es7210_inited != 0u) {
        return 0;
    }
    if (ops == NULL ||
        ops->i2c_write_reg == NULL ||
        ops->i2c_read_reg == NULL ||
        ops->i2s_read == NULL) {
        D_LOGE(TAG, "ES7210 板级初始化失败: WDRIVER 能力无效");
        return -1;
    }

    s_d_ops = *ops;
    s_d_read_log_count = 0u;
    s_d_channel_log_count = 0u;
    int ret = es7210_config_default();
    if (ret != 0) {
        memset(&s_d_ops, 0, sizeof(s_d_ops));
        return -2;
    }

    s_es7210_inited = 1u;
    D_LOGI(TAG, "ES7210 驱动初始化成功");
    return 0;
}

int d_es7210_deinit(void)
{
    if (s_es7210_inited != 0u) {
        (void)es7210_write_u8(ES7210_RESET_REG00, 0xFF);
        D_LOGI(TAG, "ES7210 驱动已释放");
    }
    s_es7210_inited = 0u;
    s_d_read_log_count = 0u;
    s_d_channel_log_count = 0u;
    memset(&s_d_ops, 0, sizeof(s_d_ops));
    return 0;
}

int d_es7210_is_initialized(void)
{
    return s_es7210_inited != 0u ? 1 : 0;
}

int d_es7210_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es7210_inited == 0u || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    int mic1_peak = 0;
    int mic2_peak = 0;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > D_ES7210_MAX_FRAMES) {
            frames = D_ES7210_MAX_FRAMES;
        }

        uint32_t read_len = frames * 4u;
        int read_bytes = es7210_read(s_d_raw_buf, read_len, timeout_ms);
        if (read_bytes < 0) {
            return read_bytes;
        }
        if (read_bytes == 0) {
            break;
        }

        uint32_t read_frames = (uint32_t)read_bytes / 4u;
        for (uint32_t i = 0; i < read_frames; i++) {
            int16_t mic1 = d_es7210_read_i16_le(s_d_raw_buf, i * 2u);
            int16_t mic2 = d_es7210_read_i16_le(s_d_raw_buf, i * 2u + 1u);
            int mic1_abs = d_es7210_abs_i16(mic1);
            int mic2_abs = d_es7210_abs_i16(mic2);
            if (mic1_abs > mic1_peak) {
                mic1_peak = mic1_abs;
            }
            if (mic2_abs > mic2_peak) {
                mic2_peak = mic2_abs;
            }

#if D_ES7210_CAPTURE_SELECT == D_ES7210_CAPTURE_MIC2
            pcm[total + i] = mic2;
#elif D_ES7210_CAPTURE_SELECT == D_ES7210_CAPTURE_MIX12
            pcm[total + i] = (int16_t)(((int32_t)mic1 + (int32_t)mic2) / 2);
#else
            pcm[total + i] = mic1;
#endif
        }
        total += read_frames;

        if ((uint32_t)read_bytes < read_len) {
            break;
        }
    }

    if (total > 0u) {
        s_d_channel_log_count++;
        if ((s_d_channel_log_count % 50u) == 0u) {
            D_LOGI(TAG, "ES7210 通道峰值, mic1=%d, mic2=%d, 输出通道=%d",
                   mic1_peak,
                   mic2_peak,
                   D_ES7210_CAPTURE_SELECT);
        }
    }

    return total > 0u ? (int)total : 0;
}
