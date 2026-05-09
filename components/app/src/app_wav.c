/**
 * @file app_wav.c
 * @brief PCM/WAV 打包和解析工具。
 */
#include "app_wav.h"

#include "app_business_config.h"

#include <string.h>

static void app_wav_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t app_wav_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static void app_wav_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t app_wav_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

int app_wav_find_riff(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len < 4u) {
        return -1;
    }

    for (uint16_t i = 0; i <= (uint16_t)(len - 4u); i++) {
        if (memcmp(&buf[i], "RIFF", 4u) == 0) {
            return (int)i;
        }
    }

    return -1;
}

void app_wav_write_header(uint8_t *buf, uint32_t pcm_bytes)
{
    uint32_t byte_rate = APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint16_t block_align = APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint32_t riff_size = 36u + pcm_bytes;

    memcpy(&buf[0], "RIFF", 4u);
    app_wav_write_u32(&buf[4], riff_size);
    memcpy(&buf[8], "WAVEfmt ", 8u);
    app_wav_write_u32(&buf[16], 16u);
    app_wav_write_u16(&buf[20], 1u);
    app_wav_write_u16(&buf[22], APP_BUSINESS_AUDIO_CHANNELS);
    app_wav_write_u32(&buf[24], APP_BUSINESS_AUDIO_SAMPLE_RATE);
    app_wav_write_u32(&buf[28], byte_rate);
    app_wav_write_u16(&buf[32], block_align);
    app_wav_write_u16(&buf[34], APP_BUSINESS_AUDIO_BITS);
    memcpy(&buf[36], "data", 4u);
    app_wav_write_u32(&buf[40], pcm_bytes);
}

int app_wav_parse_pcm16_mono_16k(const uint8_t *wav,
                                 uint16_t wav_len,
                                 const int16_t **pcm,
                                 uint32_t *samples)
{
    if (wav == NULL || wav_len < APP_BUSINESS_WAV_HEADER_LEN || pcm == NULL || samples == NULL) {
        return -1;
    }
    if (memcmp(&wav[0], "RIFF", 4u) != 0 || memcmp(&wav[8], "WAVE", 4u) != 0) {
        return -2;
    }

    uint16_t audio_format = app_wav_read_u16(&wav[20]);
    uint16_t channels = app_wav_read_u16(&wav[22]);
    uint32_t sample_rate = app_wav_read_u32(&wav[24]);
    uint16_t bits = app_wav_read_u16(&wav[34]);

    if (audio_format != 1u || channels != APP_BUSINESS_AUDIO_CHANNELS ||
        sample_rate != APP_BUSINESS_AUDIO_SAMPLE_RATE || bits != APP_BUSINESS_AUDIO_BITS) {
        return -3;
    }

    uint32_t data_offset = 36u;
    while ((data_offset + 8u) <= wav_len) {
        uint32_t chunk_size = app_wav_read_u32(&wav[data_offset + 4u]);
        if (memcmp(&wav[data_offset], "data", 4u) == 0) {
            if ((data_offset + 8u + chunk_size) > wav_len) {
                return -4;
            }
            *pcm = (const int16_t *)&wav[data_offset + 8u];
            *samples = chunk_size / sizeof(int16_t);
            return 0;
        }
        data_offset += 8u + chunk_size + (chunk_size & 1u);
    }

    return -5;
}
