/**
 * @file driver_es8311.c
 * @brief ES8311 低功耗单声道音频 CODEC 驱动实现。
 */
#include "driver_es8311.h"

#include "bsp_common.h"
#include "osal_task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "driver_es8311";

#define ES8311_RESET_REG00       0x00u
#define ES8311_CLK_MANAGER_REG01 0x01u
#define ES8311_CLK_MANAGER_REG02 0x02u
#define ES8311_CLK_MANAGER_REG03 0x03u
#define ES8311_CLK_MANAGER_REG04 0x04u
#define ES8311_CLK_MANAGER_REG05 0x05u
#define ES8311_CLK_MANAGER_REG06 0x06u
#define ES8311_CLK_MANAGER_REG07 0x07u
#define ES8311_CLK_MANAGER_REG08 0x08u
#define ES8311_SDPIN_REG09       0x09u
#define ES8311_SDPOUT_REG0A      0x0Au
#define ES8311_SYSTEM_REG0D      0x0Du
#define ES8311_SYSTEM_REG0E      0x0Eu
#define ES8311_SYSTEM_REG12      0x12u
#define ES8311_SYSTEM_REG13      0x13u
#define ES8311_SYSTEM_REG14      0x14u
#define ES8311_ADC_REG16         0x16u
#define ES8311_ADC_REG17         0x17u
#define ES8311_ADC_REG1C         0x1Cu
#define ES8311_DAC_REG31         0x31u
#define ES8311_DAC_REG32         0x32u
#define ES8311_DAC_REG37         0x37u
#define DRIVER_ES8311_I2C_SPEED_HZ 100000u
#define DRIVER_ES8311_I2C_ADDR     0x18u
#define DRIVER_ES8311_MAX_FRAMES   256u

struct es8311_dev {
    es8311_interface_t itf;
    uint32_t play_log_count;
};

static es8311_handle_t s_es8311 = NULL;
static driver_es8311_bsp_ops_t s_driver_ops;
static int16_t s_driver_stereo_buf[DRIVER_ES8311_MAX_FRAMES * 2u];

static int es8311_write_u8(es8311_handle_t dev, uint8_t reg, uint8_t value)
{
    return dev->itf.write_reg(reg, &value, 1u);
}

static int es8311_config_default(es8311_handle_t dev)
{
    int ret = es8311_write_u8(dev, ES8311_RESET_REG00, 0x1F);
    if (ret != 0) {
        return ret;
    }
    osal_delay_ms(20);

    static const struct {
        uint8_t reg;
        uint8_t value;
    } init_seq[] = {
        {ES8311_RESET_REG00, 0x00},
        {ES8311_RESET_REG00, 0x80},
        {ES8311_CLK_MANAGER_REG01, 0x3F}, /* enable clocks, MCLK pin source */
        {ES8311_CLK_MANAGER_REG02, 0x00}, /* 16 kHz, MCLK=4.096 MHz */
        {ES8311_CLK_MANAGER_REG03, 0x10},
        {ES8311_CLK_MANAGER_REG04, 0x10},
        {ES8311_CLK_MANAGER_REG05, 0x00},
        {ES8311_CLK_MANAGER_REG06, 0x03},
        {ES8311_CLK_MANAGER_REG07, 0x00},
        {ES8311_CLK_MANAGER_REG08, 0xFF},
        {ES8311_SDPIN_REG09, 0x0C},  /* I2S input, 16 bit */
        {ES8311_SDPOUT_REG0A, 0x0C}, /* I2S output, 16 bit */
        {ES8311_SYSTEM_REG0D, 0x01},
        {ES8311_SYSTEM_REG0E, 0x02},
        {ES8311_SYSTEM_REG12, 0x00},
        {ES8311_SYSTEM_REG13, 0x10},
        {ES8311_SYSTEM_REG14, 0x1A},
        {ES8311_ADC_REG16, 0x00},
        {ES8311_ADC_REG17, 0xC8},
        {ES8311_ADC_REG1C, 0x6A},
        {ES8311_DAC_REG31, 0x00},
        {ES8311_DAC_REG32, 0xBF},
        {ES8311_DAC_REG37, 0x08},
    };

    for (unsigned int i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        ret = es8311_write_u8(dev, init_seq[i].reg, init_seq[i].value);
        if (ret != 0) {
            BSP_LOGE(TAG, "ES8311 寄存器配置失败, reg=0x%02X, ret=%d",
                     (unsigned int)init_seq[i].reg, ret);
            return ret;
        }
    }

    BSP_LOGI(TAG, "ES8311 芯片配置完成, sample_rate=16000, bits=16");
    return 0;
}

es8311_handle_t es8311_init(es8311_interface_t *itf)
{
    if (itf == NULL || itf->write_reg == NULL || itf->read_reg == NULL || itf->write == NULL) {
        BSP_LOGE(TAG, "ES8311 初始化失败: 底层能力为空");
        return NULL;
    }

    es8311_handle_t dev = (es8311_handle_t)calloc(1, sizeof(struct es8311_dev));
    if (dev == NULL) {
        BSP_LOGE(TAG, "ES8311 初始化失败: 内存分配失败");
        return NULL;
    }

    dev->itf = *itf;
    if (es8311_config_default(dev) != 0) {
        free(dev);
        return NULL;
    }

    BSP_LOGI(TAG, "ES8311 驱动初始化成功");
    return dev;
}

void es8311_deinit(es8311_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    (void)es8311_write_u8(dev, ES8311_RESET_REG00, 0x1F);
    free(dev);
    BSP_LOGI(TAG, "ES8311 驱动已释放");
}

int es8311_play(es8311_handle_t dev,
                const uint8_t *data,
                uint32_t len,
                uint32_t timeout_ms)
{
    if (dev == NULL || data == NULL || len == 0) {
        BSP_LOGE(TAG, "ES8311 播放参数无效, dev=%p, data=%p, len=%u",
                 dev, data, (unsigned int)len);
        return -1;
    }

    int ret = dev->itf.write(data, len, timeout_ms);
    if (ret >= 0) {
        dev->play_log_count++;
        if ((dev->play_log_count % 100u) != 0u) {
            return ret;
        }
        BSP_LOGI(TAG, "ES8311 播放写入完成, request=%u, written=%d",
                 (unsigned int)len, ret);
    } else {
        BSP_LOGE(TAG, "ES8311 播放写入失败, ret=%d", ret);
    }

    return ret;
}

int es8311_set_volume(es8311_handle_t dev, uint8_t volume)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "ES8311 设置音量失败: 句柄为空");
        return -1;
    }

    if (volume > 100u) {
        volume = 100u;
    }

    uint8_t reg_value = (volume == 0u) ? 0u : (uint8_t)(((uint32_t)volume * 256u / 100u) - 1u);
    int ret = es8311_write_u8(dev, ES8311_DAC_REG32, reg_value);
    if (ret == 0) {
        BSP_LOGI(TAG, "ES8311 音量设置成功, volume=%u", (unsigned int)volume);
    } else {
        BSP_LOGE(TAG, "ES8311 音量设置失败, ret=%d", ret);
    }

    return ret;
}

int es8311_set_mute(es8311_handle_t dev, int mute)
{
    if (dev == NULL) {
        BSP_LOGE(TAG, "ES8311 设置静音失败: 句柄为空");
        return -1;
    }

    uint8_t reg_value = 0;
    int ret = dev->itf.read_reg(ES8311_DAC_REG31, &reg_value, 1u);
    if (ret != 0) {
        BSP_LOGE(TAG, "ES8311 读取静音寄存器失败, ret=%d", ret);
        return ret;
    }

    if (mute) {
        reg_value |= 0x60u;
    } else {
        reg_value &= (uint8_t)~0x60u;
    }

    ret = es8311_write_u8(dev, ES8311_DAC_REG31, reg_value);
    if (ret == 0) {
        BSP_LOGI(TAG, "ES8311 静音设置成功, mute=%d", mute ? 1 : 0);
    } else {
        BSP_LOGE(TAG, "ES8311 静音设置失败, ret=%d", ret);
    }

    return ret;
}

static int driver_es8311_write_reg(uint8_t reg, const uint8_t *data, uint16_t len)
{
    return s_driver_ops.i2c_write_reg(DRIVER_ES8311_I2C_ADDR,
                                      DRIVER_ES8311_I2C_SPEED_HZ,
                                      reg,
                                      data,
                                      len);
}

static int driver_es8311_read_reg(uint8_t reg, uint8_t *data, uint16_t len)
{
    return s_driver_ops.i2c_read_reg(DRIVER_ES8311_I2C_ADDR,
                                     DRIVER_ES8311_I2C_SPEED_HZ,
                                     reg,
                                     data,
                                     len);
}

int driver_es8311_init(const driver_es8311_bsp_ops_t *ops)
{
    if (s_es8311 != NULL) {
        return 0;
    }
    if (ops == NULL ||
        ops->i2c_write_reg == NULL ||
        ops->i2c_read_reg == NULL ||
        ops->i2s_write == NULL) {
        BSP_LOGE(TAG, "ES8311 板级初始化失败: BSP 能力无效");
        return -1;
    }

    s_driver_ops = *ops;
    es8311_interface_t itf = {
        .write_reg = driver_es8311_write_reg,
        .read_reg = driver_es8311_read_reg,
        .write = s_driver_ops.i2s_write,
    };

    s_es8311 = es8311_init(&itf);
    return s_es8311 != NULL ? 0 : -2;
}

int driver_es8311_deinit(void)
{
    if (s_es8311 != NULL) {
        es8311_deinit(s_es8311);
        s_es8311 = NULL;
    }
    memset(&s_driver_ops, 0, sizeof(s_driver_ops));
    return 0;
}

int driver_es8311_is_initialized(void)
{
    return s_es8311 != NULL ? 1 : 0;
}

int driver_es8311_start_playback(void)
{
    return s_es8311 != NULL ? 0 : -1;
}

int driver_es8311_stop_playback(void)
{
    return s_es8311 != NULL ? 0 : -1;
}

int driver_es8311_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es8311 == NULL || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > DRIVER_ES8311_MAX_FRAMES) {
            frames = DRIVER_ES8311_MAX_FRAMES;
        }

        for (uint32_t i = 0; i < frames; i++) {
            s_driver_stereo_buf[i * 2u] = pcm[total + i];
            s_driver_stereo_buf[i * 2u + 1u] = pcm[total + i];
        }

        uint32_t write_len = frames * 4u;
        int written = es8311_play(s_es8311, (const uint8_t *)s_driver_stereo_buf, write_len, timeout_ms);
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

int driver_es8311_set_volume(uint8_t volume)
{
    return s_es8311 != NULL ? es8311_set_volume(s_es8311, volume) : -1;
}

int driver_es8311_set_mute(int mute)
{
    return s_es8311 != NULL ? es8311_set_mute(s_es8311, mute) : -1;
}
