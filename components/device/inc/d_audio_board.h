/**
 * @file d_audio_board.h
 * @brief WDRIVER 音频板级控制接口。
 */
#ifndef D_AUDIO_BOARD_H
#define D_AUDIO_BOARD_H

#include "d_config.h"
#include "wdriver_config.h"

#define d_audio_board_codec_ENABLE_IO WDRIVER_AUDIO_CODEC_ENABLE_IO

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打开音频 codec 板供电/使能。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_audio_board_codec_power_on(void);

/**
 * @brief 关闭音频 codec 板供电/使能。
 *
 * @return 成功返回 0；失败返回负值。
 */
int d_audio_board_codec_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* D_AUDIO_BOARD_H */
