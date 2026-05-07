/**
 * @file bsp_audio.h
 * @brief BSP 音频板级控制接口。
 */
#ifndef BSP_AUDIO_H
#define BSP_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打开音频 codec 板供电/使能。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_audio_codec_power_on(void);

/**
 * @brief 关闭音频 codec 板供电/使能。
 *
 * @return 成功返回 0；失败返回负值。
 */
int bsp_audio_codec_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_AUDIO_H */
