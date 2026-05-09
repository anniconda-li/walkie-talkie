/**
 * @file driver_max98357a.h
 * @brief MAX98357A I2S 数字功放驱动接口。
 */
#ifndef DRIVER_MAX98357A_H
#define DRIVER_MAX98357A_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MAX98357A 驱动初始化所需的 BSP 能力。
 */
typedef struct {
    int (*i2s_write)(const uint8_t *data,
                     uint32_t len,
                     uint32_t timeout_ms); /**< I2S 写入能力。 */
} driver_max98357a_bsp_ops_t;

/**
 * @brief 初始化 MAX98357A 播放驱动。
 *
 * @param[in] ops BSP 能力函数表。
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_init(const driver_max98357a_bsp_ops_t *ops);

/**
 * @brief 释放 MAX98357A 播放驱动。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_deinit(void);

/**
 * @brief 判断 MAX98357A 播放驱动是否已初始化。
 *
 * @return 已初始化返回 1；未初始化返回 0。
 */
int driver_max98357a_is_initialized(void);

/**
 * @brief 开始播放。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_start_playback(void);

/**
 * @brief 停止播放。
 *
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_stop_playback(void);

/**
 * @brief 播放单声道 PCM 样本。
 *
 * @param[in] pcm 单声道 PCM 输入缓冲区。
 * @param[in] samples 样本数。
 * @param[in] timeout_ms 写入超时时间，单位毫秒。
 * @return 实际播放样本数；失败返回负值。
 */
int driver_max98357a_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

/**
 * @brief 设置软件音量。
 *
 * @param[in] volume 音量百分比。
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_set_volume(uint8_t volume);

/**
 * @brief 设置软件静音状态。
 *
 * @param[in] mute 非 0 静音，0 取消静音。
 * @return 成功返回 0；失败返回负值。
 */
int driver_max98357a_set_mute(int mute);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_MAX98357A_H */
