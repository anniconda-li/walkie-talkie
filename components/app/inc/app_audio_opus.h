/**
 * @file app_audio_opus.h
 * @brief App 层共享的 16 kHz 单声道 Opus 编码器。
 */
#ifndef APP_AUDIO_OPUS_H
#define APP_AUDIO_OPUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_AUDIO_OPUS_FRAME_SAMPLES     320u
#define APP_AUDIO_OPUS_PACKET_MAX_BYTES  1275u

/** @brief 幂等初始化唯一的 App 层 Opus encoder。 */
int app_audio_opus_encoder_init(void);

/** @brief 在一次新的 PTT 或 AI 录音开始前重置共享 encoder。 */
int app_audio_opus_encoder_reset(void);

/** @brief 将一帧 20 ms、320 samples 的 PCM 编码为裸 Opus packet。 */
int app_audio_opus_encode_20ms(const int16_t *pcm,
                               uint16_t samples,
                               uint8_t *output,
                               uint16_t output_capacity,
                               uint16_t *output_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_AUDIO_OPUS_H */
