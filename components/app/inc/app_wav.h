/**
 * @file app_wav.h
 * @brief PCM/WAV 打包和解析工具。
 */
#ifndef APP_WAV_H
#define APP_WAV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_wav_find_riff(const uint8_t *buf, uint16_t len);
void app_wav_write_header(uint8_t *buf, uint32_t pcm_bytes);
int app_wav_parse_pcm16_mono_16k(const uint8_t *wav,
                                 uint16_t wav_len,
                                 const int16_t **pcm,
                                 uint32_t *samples);

#ifdef __cplusplus
}
#endif

#endif /* APP_WAV_H */
