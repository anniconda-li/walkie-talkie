/**
 * @file bsp_es8311.c
 * @brief ES8311 低功耗单声道音频 CODEC 驱动实现。
 */
#include "bsp_es8311.h"

#include "bsp_common.h"
#include "osal_task.h"

#include <stdlib.h>

static const char *TAG = "bsp_es8311";

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

struct es8311_dev {
    es8311_interface_t itf;
    uint32_t play_log_count;
};

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
