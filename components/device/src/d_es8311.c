/**
 * @file d_es8311.c
 * @brief ES8311 低功耗单声道音频 CODEC 驱动实现。
 */
#include "d_es8311.h"

#include "d_config.h"
#include "osal_task.h"

#include <string.h>

static const char *TAG = "d_es8311";

#define ES8311_RESET_REG00       0x00u /**< 复位和芯片控制寄存器。 */
#define ES8311_CLK_MANAGER_REG01 0x01u /**< 时钟管理寄存器 1。 */
#define ES8311_CLK_MANAGER_REG02 0x02u /**< 时钟管理寄存器 2。 */
#define ES8311_CLK_MANAGER_REG03 0x03u /**< 时钟管理寄存器 3。 */
#define ES8311_CLK_MANAGER_REG04 0x04u /**< 时钟管理寄存器 4。 */
#define ES8311_CLK_MANAGER_REG05 0x05u /**< 时钟管理寄存器 5。 */
#define ES8311_CLK_MANAGER_REG06 0x06u /**< 时钟管理寄存器 6。 */
#define ES8311_CLK_MANAGER_REG07 0x07u /**< 时钟管理寄存器 7。 */
#define ES8311_CLK_MANAGER_REG08 0x08u /**< 时钟管理寄存器 8。 */
#define ES8311_SDPIN_REG09       0x09u /**< 串行音频输入接口配置寄存器。 */
#define ES8311_SDPOUT_REG0A      0x0Au /**< 串行音频输出接口配置寄存器。 */
#define ES8311_SYSTEM_REG0D      0x0Du /**< 系统模拟通路控制寄存器 0D。 */
#define ES8311_SYSTEM_REG0E      0x0Eu /**< 系统模拟通路控制寄存器 0E。 */
#define ES8311_SYSTEM_REG12      0x12u /**< 系统电源控制寄存器 12。 */
#define ES8311_SYSTEM_REG13      0x13u /**< 系统电源控制寄存器 13。 */
#define ES8311_SYSTEM_REG14      0x14u /**< 系统电源控制寄存器 14。 */
#define ES8311_ADC_REG16         0x16u /**< ADC 配置寄存器 16。 */
#define ES8311_ADC_REG17         0x17u /**< ADC 配置寄存器 17。 */
#define ES8311_ADC_REG1C         0x1Cu /**< ADC 音量和自动控制寄存器。 */
#define ES8311_DAC_REG31         0x31u /**< DAC 静音和输出控制寄存器。 */
#define ES8311_DAC_REG32         0x32u /**< DAC 数字音量寄存器。 */
#define ES8311_DAC_REG37         0x37u /**< DAC 输出功放控制寄存器。 */

#define ES8311_DAC_MUTE_MASK     0x60u
#define ES8311_DAC_REG37_DEFAULT 0x08u

/** @brief ES8311 驱动是否已完成初始化。 */
static uint8_t s_es8311_inited = 0u;

/** @brief 初始化时承接并保存的 WDRIVER 能力函数表。 */
static d_es8311_wdriver_ops_t s_d_ops;

/** @brief ES8311 单声道转双声道播放缓存。 */
static int16_t s_d_stereo_buf[D_ES8311_MAX_FRAMES * 2u];

/** @brief 用户设置的播放音量百分比，0 表示用户主动静音。 */
static uint8_t s_es8311_volume = 80u;

/** @brief 播放会话是否因 stop_playback 临时静音。 */
static uint8_t s_es8311_playback_muted = 1u;

/**
 * @brief 向 ES8311 写入一个 8 位寄存器值。
 */
static int es8311_write_u8(uint8_t reg, uint8_t value)
{
    return s_d_ops.i2c_write_reg(D_ES8311_I2C_ADDR,
                                      D_ES8311_I2C_SPEED_HZ,
                                      reg,
                                      &value,
                                      1u);
}

/**
 * @brief 写入 ES8311 默认初始化寄存器序列。
 */
static int es8311_config_default(void)
{
    int ret = es8311_write_u8(ES8311_RESET_REG00, 0x1F);
    if (ret != 0) {
        return ret;
    }
    osal_delay_ms(20);

    static const struct {
        uint8_t reg;
        uint8_t value;
    } init_seq[] = {
        /* 复位芯片并重新打开寄存器控制。 */
        {ES8311_RESET_REG00, 0x00},
        {ES8311_RESET_REG00, 0x80},

        /* 配置 16 kHz 播放所需的时钟树和分频。 */
        {ES8311_CLK_MANAGER_REG01, 0x3F}, /* 使能时钟，MCLK 来自外部引脚。 */
        {ES8311_CLK_MANAGER_REG02, 0x00}, /* 16 kHz，MCLK=4.096 MHz。 */
        {ES8311_CLK_MANAGER_REG03, 0x10},
        {ES8311_CLK_MANAGER_REG04, 0x10},
        {ES8311_CLK_MANAGER_REG05, 0x00},
        {ES8311_CLK_MANAGER_REG06, 0x03},
        {ES8311_CLK_MANAGER_REG07, 0x00},
        {ES8311_CLK_MANAGER_REG08, 0xFF},

        /* 配置 I2S 输入输出格式。 */
        {ES8311_SDPIN_REG09, 0x0C},  /* I2S 输入，16 bit。 */
        {ES8311_SDPOUT_REG0A, 0x0C}, /* I2S 输出，16 bit。 */

        /* 打开系统模拟通路和 ADC 侧基础配置。 */
        {ES8311_SYSTEM_REG0D, 0x01},
        {ES8311_SYSTEM_REG0E, 0x02},
        {ES8311_SYSTEM_REG12, 0x00},
        {ES8311_SYSTEM_REG13, 0x10},
        {ES8311_SYSTEM_REG14, 0x1A},
        {ES8311_ADC_REG16, 0x00},
        {ES8311_ADC_REG17, 0xC8},
        {ES8311_ADC_REG1C, 0x6A},

        /* 配置 DAC 输出、默认音量和功放输出状态。 */
        {ES8311_DAC_REG31, 0x00},
        {ES8311_DAC_REG32, 0xBF},
        {ES8311_DAC_REG37, ES8311_DAC_REG37_DEFAULT},
    };

    for (unsigned int i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        ret = es8311_write_u8(init_seq[i].reg, init_seq[i].value);
        if (ret != 0) {
            D_LOGE(TAG, "ES8311 寄存器配置失败, reg=0x%02X, ret=%d",
                     (unsigned int)init_seq[i].reg, ret);
            return ret;
        }
    }

    D_LOGI(TAG, "ES8311 芯片配置完成, sample_rate=16000, bits=16");
    return 0;
}

/**
 * @brief 写入一段 ES8311 播放 PCM 数据。
 */
static int es8311_play(const uint8_t *data,
                       uint32_t len,
                       uint32_t timeout_ms)
{
    if (s_es8311_inited == 0u || data == NULL || len == 0) {
        D_LOGE(TAG, "ES8311 播放参数无效, inited=%u, data=%p, len=%u",
                 (unsigned int)s_es8311_inited, data, (unsigned int)len);
        return -1;
    }

    int ret = s_d_ops.i2s_write(data, len, timeout_ms);
    if (ret < 0) {
        D_LOGE(TAG, "ES8311 播放写入失败, ret=%d", ret);
    }

    return ret;
}

/**
 * @brief 将用户百分比音量映射为 ES8311 DAC 数字音量寄存器值。
 *
 * UI 仍使用 0-100 线性滑块，driver 内部用 10% 锚点插值提供更接近听感的曲线：
 * 低段抬高并拉开 10%-20% 差距，避免最低档接近静音；高段压缩，避免每档跳变过大。
 */
static uint8_t es8311_volume_to_dac_reg(uint8_t volume)
{
    static const uint8_t volume_curve[] = {
        0x00u, /*   0% */
        0x9Au, /*  10% */
        0xB0u, /*  20% */
        0xBEu, /*  30% */
        0xC8u, /*  40% */
        0xD0u, /*  50% */
        0xD8u, /*  60% */
        0xDEu, /*  70% */
        0xE4u, /*  80% */
        0xEAu, /*  90% */
        0xF0u, /* 100% */
    };

    if (volume == 0u) {
        return 0x00u;
    }
    if (volume > 100u) {
        volume = 100u;
    }

    uint8_t index = volume / 10u;
    uint8_t rem = volume % 10u;
    if (index >= 10u || rem == 0u) {
        return volume_curve[index];
    }

    uint8_t low = volume_curve[index];
    uint8_t high = volume_curve[index + 1u];
    return (uint8_t)(low + ((((uint16_t)(high - low) * rem) + 5u) / 10u));
}

static int es8311_apply_volume(uint8_t volume)
{
    if (s_es8311_inited == 0u) {
        D_LOGE(TAG, "ES8311 设置音量失败: 未初始化");
        return -1;
    }

    if (volume > 100u) {
        volume = 100u;
    }

    uint8_t reg_value = es8311_volume_to_dac_reg(volume);
    int ret = es8311_write_u8(ES8311_DAC_REG32, reg_value);
    if (ret != 0) {
        D_LOGE(TAG, "ES8311 音量设置失败, ret=%d", ret);
    }

    return ret;
}

/**
 * @brief 通过 ES8311 DAC 控制寄存器设置或取消静音。
 */
static int es8311_set_mute(int mute)
{
    if (s_es8311_inited == 0u) {
        D_LOGE(TAG, "ES8311 设置静音失败: 未初始化");
        return -1;
    }

    uint8_t reg_value = 0;
    int ret = s_d_ops.i2c_read_reg(D_ES8311_I2C_ADDR,
                                        D_ES8311_I2C_SPEED_HZ,
                                        ES8311_DAC_REG31,
                                        &reg_value,
                                        1u);
    if (ret != 0) {
        D_LOGE(TAG, "ES8311 读取静音寄存器失败, ret=%d", ret);
        return ret;
    }

    if (mute) {
        reg_value |= ES8311_DAC_MUTE_MASK;
    } else {
        reg_value &= (uint8_t)~ES8311_DAC_MUTE_MASK;
    }

    ret = es8311_write_u8(ES8311_DAC_REG31, reg_value);
    if (ret == 0) {
        D_LOGI(TAG, "ES8311 静音设置成功, mute=%d", mute ? 1 : 0);
    } else {
        D_LOGE(TAG, "ES8311 静音设置失败, ret=%d", ret);
    }

    return ret;
}

static void es8311_write_silence_tail(void)
{
    memset(s_d_stereo_buf, 0, sizeof(s_d_stereo_buf));
    (void)es8311_play((const uint8_t *)s_d_stereo_buf,
                      (uint32_t)sizeof(s_d_stereo_buf),
                      50u);
}

int d_es8311_init(const d_es8311_wdriver_ops_t *ops)
{
    if (s_es8311_inited != 0u) {
        return 0;
    }
    if (ops == NULL ||
        ops->i2c_write_reg == NULL ||
        ops->i2c_read_reg == NULL ||
        ops->i2s_write == NULL) {
        D_LOGE(TAG, "ES8311 板级初始化失败: WDRIVER 能力无效");
        return -1;
    }

    s_d_ops = *ops;
    int ret = es8311_config_default();
    if (ret != 0) {
        memset(&s_d_ops, 0, sizeof(s_d_ops));
        return -2;
    }

    s_es8311_inited = 1u;
    D_LOGI(TAG, "ES8311 驱动初始化成功");
    return 0;
}

int d_es8311_deinit(void)
{
    if (s_es8311_inited != 0u) {
        (void)es8311_write_u8(ES8311_RESET_REG00, 0x1F);
        D_LOGI(TAG, "ES8311 驱动已释放");
    }
    s_es8311_inited = 0u;
    memset(&s_d_ops, 0, sizeof(s_d_ops));
    return 0;
}

int d_es8311_is_initialized(void)
{
    return s_es8311_inited != 0u ? 1 : 0;
}

int d_es8311_start_playback(void)
{
    if (s_es8311_inited == 0u) {
        return -1;
    }

    s_es8311_playback_muted = 0u;
    int ret = es8311_set_mute(s_es8311_volume == 0u);
    if (ret == 0) {
        D_LOGI(TAG, "ES8311 播放输出已打开");
    }
    return ret;
}

int d_es8311_stop_playback(void)
{
    if (s_es8311_inited == 0u) {
        return -1;
    }

    s_es8311_playback_muted = 1u;
    int ret = es8311_set_mute(1);
    es8311_write_silence_tail();
    if (ret == 0) {
        D_LOGI(TAG, "ES8311 播放输出已关闭");
    }
    return ret;
}

int d_es8311_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es8311_inited == 0u || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > D_ES8311_MAX_FRAMES) {
            frames = D_ES8311_MAX_FRAMES;
        }

        for (uint32_t i = 0; i < frames; i++) {
            s_d_stereo_buf[i * 2u] = pcm[total + i];
            s_d_stereo_buf[i * 2u + 1u] = pcm[total + i];
        }

        uint32_t write_len = frames * 4u;
        int written = es8311_play((const uint8_t *)s_d_stereo_buf, write_len, timeout_ms);
        if (written < 0) {
            return written;
        }
        if (written == 0) {
            break;
        }

        total += (uint32_t)written / 4u;
        if ((uint32_t)written < write_len) {
            break;
        }
    }

    return total > 0u ? (int)total : 0;
}

int d_es8311_set_volume(uint8_t volume)
{
    if (s_es8311_inited == 0u) {
        return -1;
    }
    if (volume > 100u) {
        volume = 100u;
    }

    int ret = es8311_apply_volume(volume);
    if (ret != 0) {
        return ret;
    }

    s_es8311_volume = volume;
    if (s_es8311_playback_muted == 0u) {
        ret = es8311_set_mute(volume == 0u);
    }
    return ret;
}

int d_es8311_set_mute(int mute)
{
    if (s_es8311_inited == 0u) {
        return -1;
    }

    s_es8311_playback_muted = mute ? 1u : 0u;
    return es8311_set_mute(mute || s_es8311_volume == 0u);
}
