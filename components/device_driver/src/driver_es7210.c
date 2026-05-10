/**
 * @file driver_es7210.c
 * @brief ES7210 四通道音频 ADC 驱动实现。
 */
#include "driver_es7210.h"

#include "driver_config.h"
#include "osal_task.h"

#include <string.h>

static const char *TAG = "driver_es7210";

#define ES7210_RESET_REG00          0x00u
#define ES7210_MAINCLK_REG02        0x02u
#define ES7210_LRCK_DIVH_REG04      0x04u
#define ES7210_LRCK_DIVL_REG05      0x05u
#define ES7210_POWER_DOWN_REG06     0x06u
#define ES7210_OSR_REG07            0x07u
#define ES7210_TIME_CONTROL0_REG09  0x09u
#define ES7210_TIME_CONTROL1_REG0A  0x0Au
#define ES7210_SDP_INTERFACE1_REG11 0x11u
#define ES7210_SDP_INTERFACE2_REG12 0x12u
#define ES7210_ADC1_VOLUME_REG1B    0x1Bu
#define ES7210_ADC2_VOLUME_REG1C    0x1Cu
#define ES7210_ADC3_VOLUME_REG1D    0x1Du
#define ES7210_ADC4_VOLUME_REG1E    0x1Eu
#define ES7210_ADC34_HPF2_REG20     0x20u
#define ES7210_ADC34_HPF1_REG21     0x21u
#define ES7210_ADC12_HPF2_REG22     0x22u
#define ES7210_ADC12_HPF1_REG23     0x23u
#define ES7210_ANALOG_REG40         0x40u
#define ES7210_MIC12_BIAS_REG41     0x41u
#define ES7210_MIC34_BIAS_REG42     0x42u
#define ES7210_MIC1_GAIN_REG43      0x43u
#define ES7210_MIC2_GAIN_REG44      0x44u
#define ES7210_MIC3_GAIN_REG45      0x45u
#define ES7210_MIC4_GAIN_REG46      0x46u
#define ES7210_MIC1_POWER_REG47     0x47u
#define ES7210_MIC2_POWER_REG48     0x48u
#define ES7210_MIC3_POWER_REG49     0x49u
#define ES7210_MIC4_POWER_REG4A     0x4Au
#define ES7210_MIC12_POWER_REG4B    0x4Bu
#define ES7210_MIC34_POWER_REG4C    0x4Cu
#define DRIVER_ES7210_I2C_SPEED_HZ  100000u
#define DRIVER_ES7210_I2C_ADDR      0x40u
#define DRIVER_ES7210_I2C_ADDR_ALT  0x20u
#define DRIVER_ES7210_MAX_FRAMES    256u

typedef struct {
    int (*write_reg)(uint8_t reg, const uint8_t *data, uint16_t len);
    int (*read_reg)(uint8_t reg, uint8_t *data, uint16_t len);
    int (*read)(uint8_t *data, uint32_t len, uint32_t timeout_ms);
} es7210_interface_t;

struct es7210_dev {
    es7210_interface_t itf;
    uint32_t read_log_count;
};

static struct es7210_dev s_es7210;
static uint8_t s_es7210_inited = 0u;
static driver_es7210_bsp_ops_t s_driver_ops;
static uint16_t s_driver_addr = DRIVER_ES7210_I2C_ADDR;
static uint8_t s_driver_raw_buf[DRIVER_ES7210_MAX_FRAMES * 4u];

static int es7210_write_u8(uint8_t reg, uint8_t value)
{
    return s_es7210.itf.write_reg(reg, &value, 1u);
}

static int es7210_config_default(void)
{
    static const struct {
        uint8_t reg;
        uint8_t value;
        uint16_t delay_ms;
    } init_seq[] = {
        {ES7210_RESET_REG00, 0xFF},
        {ES7210_RESET_REG00, 0x32, 20u},
        {ES7210_TIME_CONTROL0_REG09, 0x30},
        {ES7210_TIME_CONTROL1_REG0A, 0x30},
        {ES7210_ADC12_HPF1_REG23, 0x2A},
        {ES7210_ADC12_HPF2_REG22, 0x0A},
        {ES7210_ADC34_HPF1_REG21, 0x2A},
        {ES7210_ADC34_HPF2_REG20, 0x0A},
        {ES7210_SDP_INTERFACE1_REG11, 0x60}, /* I2S, 16 bit */
        {ES7210_SDP_INTERFACE2_REG12, 0x00}, /* standard I2S, TDM disabled */
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
        {ES7210_OSR_REG07, 0x20},
        {ES7210_MAINCLK_REG02, 0xC1, 20u}, /* 16 kHz, MCLK=4.096 MHz */
        {ES7210_LRCK_DIVH_REG04, 0x01},
        {ES7210_LRCK_DIVL_REG05, 0x00},
        {ES7210_POWER_DOWN_REG06, 0x04},
        {ES7210_MIC12_POWER_REG4B, 0x0F},
        {ES7210_MIC34_POWER_REG4C, 0x00},
        {ES7210_ADC1_VOLUME_REG1B, 0xBF},
        {ES7210_ADC2_VOLUME_REG1C, 0xBF},
        {ES7210_ADC3_VOLUME_REG1D, 0xBF},
        {ES7210_ADC4_VOLUME_REG1E, 0xBF},
        {ES7210_RESET_REG00, 0x71},
        {ES7210_RESET_REG00, 0x41, 20u},
    };

    for (unsigned int i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        int ret = es7210_write_u8(init_seq[i].reg, init_seq[i].value);
        if (ret != 0) {
            DRIVER_LOGE(TAG, "ES7210 寄存器配置失败, reg=0x%02X, ret=%d",
                     (unsigned int)init_seq[i].reg, ret);
            return ret;
        }
        if (init_seq[i].delay_ms > 0u) {
            osal_delay_ms(init_seq[i].delay_ms);
        }
    }

    DRIVER_LOGI(TAG, "ES7210 芯片配置完成, sample_rate=16000, bits=16");
    return 0;
}

static int es7210_init(es7210_interface_t *itf)
{
    if (itf == NULL || itf->write_reg == NULL || itf->read_reg == NULL || itf->read == NULL) {
        DRIVER_LOGE(TAG, "ES7210 初始化失败: 底层能力为空");
        return -1;
    }

    memset(&s_es7210, 0, sizeof(s_es7210));
    s_es7210.itf = *itf;
    if (es7210_config_default() != 0) {
        memset(&s_es7210, 0, sizeof(s_es7210));
        return -2;
    }

    s_es7210_inited = 1u;
    DRIVER_LOGI(TAG, "ES7210 驱动初始化成功");
    return 0;
}

static void es7210_deinit(void)
{
    if (s_es7210_inited == 0u) {
        return;
    }

    (void)es7210_write_u8(ES7210_RESET_REG00, 0xFF);
    memset(&s_es7210, 0, sizeof(s_es7210));
    s_es7210_inited = 0u;
    DRIVER_LOGI(TAG, "ES7210 驱动已释放");
}

static int es7210_read(uint8_t *data,
                       uint32_t len,
                       uint32_t timeout_ms)
{
    if (s_es7210_inited == 0u || data == NULL || len == 0) {
        DRIVER_LOGE(TAG, "ES7210 读取参数无效, inited=%u, data=%p, len=%u",
                 (unsigned int)s_es7210_inited, data, (unsigned int)len);
        return -1;
    }

    int ret = s_es7210.itf.read(data, len, timeout_ms);
    if (ret >= 0) {
        s_es7210.read_log_count++;
        if ((s_es7210.read_log_count % 100u) != 0u) {
            return ret;
        }
        DRIVER_LOGI(TAG, "ES7210 读取完成, request=%u, read=%d",
                 (unsigned int)len, ret);
    } else {
        DRIVER_LOGE(TAG, "ES7210 读取失败, ret=%d", ret);
    }

    return ret;
}

static int driver_es7210_write_reg(uint8_t reg, const uint8_t *data, uint16_t len)
{
    int ret = s_driver_ops.i2c_write_reg(s_driver_addr,
                                         DRIVER_ES7210_I2C_SPEED_HZ,
                                         reg,
                                         data,
                                         len);
    if (ret == 0 || reg != 0x00u || s_driver_addr == DRIVER_ES7210_I2C_ADDR_ALT) {
        return ret;
    }

    s_driver_addr = DRIVER_ES7210_I2C_ADDR_ALT;
    ret = s_driver_ops.i2c_write_reg(s_driver_addr,
                                     DRIVER_ES7210_I2C_SPEED_HZ,
                                     reg,
                                     data,
                                     len);
    if (ret != 0) {
        s_driver_addr = DRIVER_ES7210_I2C_ADDR;
    }
    return ret;
}

static int driver_es7210_read_reg(uint8_t reg, uint8_t *data, uint16_t len)
{
    return s_driver_ops.i2c_read_reg(s_driver_addr,
                                     DRIVER_ES7210_I2C_SPEED_HZ,
                                     reg,
                                     data,
                                     len);
}

static int16_t driver_es7210_read_i16_le(const uint8_t *data, uint32_t sample_index)
{
    uint32_t offset = sample_index * 2u;
    uint16_t raw = (uint16_t)data[offset] | ((uint16_t)data[offset + 1u] << 8);
    return (int16_t)raw;
}

int driver_es7210_init(const driver_es7210_bsp_ops_t *ops)
{
    if (s_es7210_inited != 0u) {
        return 0;
    }
    if (ops == NULL ||
        ops->i2c_write_reg == NULL ||
        ops->i2c_read_reg == NULL ||
        ops->i2s_read == NULL) {
        DRIVER_LOGE(TAG, "ES7210 板级初始化失败: BSP 能力无效");
        return -1;
    }

    s_driver_ops = *ops;
    s_driver_addr = DRIVER_ES7210_I2C_ADDR;
    es7210_interface_t itf = {
        .write_reg = driver_es7210_write_reg,
        .read_reg = driver_es7210_read_reg,
        .read = s_driver_ops.i2s_read,
    };

    return es7210_init(&itf);
}

int driver_es7210_deinit(void)
{
    es7210_deinit();
    memset(&s_driver_ops, 0, sizeof(s_driver_ops));
    return 0;
}

int driver_es7210_is_initialized(void)
{
    return s_es7210_inited != 0u ? 1 : 0;
}

int driver_es7210_start_record(void)
{
    return s_es7210_inited != 0u ? 0 : -1;
}

int driver_es7210_stop_record(void)
{
    return s_es7210_inited != 0u ? 0 : -1;
}

int driver_es7210_read_pcm(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es7210_inited == 0u || pcm == NULL || samples == 0u) {
        return -1;
    }

    uint32_t total = 0u;
    while (total < samples) {
        uint32_t frames = samples - total;
        if (frames > DRIVER_ES7210_MAX_FRAMES) {
            frames = DRIVER_ES7210_MAX_FRAMES;
        }

        uint32_t read_len = frames * 4u;
        int read_bytes = es7210_read(s_driver_raw_buf, read_len, timeout_ms);
        if (read_bytes < 0) {
            return read_bytes;
        }
        if (read_bytes == 0) {
            break;
        }

        uint32_t read_frames = (uint32_t)read_bytes / 4u;
        for (uint32_t i = 0; i < read_frames; i++) {
            int16_t mic1 = driver_es7210_read_i16_le(s_driver_raw_buf, i * 2u);
            int16_t mic2 = driver_es7210_read_i16_le(s_driver_raw_buf, i * 2u + 1u);
            pcm[total + i] = (int16_t)(((int32_t)mic1 + (int32_t)mic2) / 2);
        }
        total += read_frames;

        if ((uint32_t)read_bytes < read_len) {
            break;
        }
    }

    return total > 0u ? (int)total : 0;
}
