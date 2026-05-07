/**
 * @file service_audio.h
 * @brief 音频能力服务接口。
 *
 * 本服务对上提供单声道 PCM 录放能力。ES7210 的 MIC1/MIC2 双通道采集和
 * ES8311 播放所需的 L/R 双声道复制都封装在 service 内部。
 */
#ifndef SERVICE_AUDIO_H
#define SERVICE_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单声道输入来源。
 */
typedef enum {
    SERVICE_AUDIO_INPUT_MIC1 = 0, /**< 只使用 MIC1。 */
    SERVICE_AUDIO_INPUT_MIC2,     /**< 只使用 MIC2。 */
    SERVICE_AUDIO_INPUT_MIX_AVG,  /**< MIC1/MIC2 平均混合。 */
} service_audio_input_t;

/**
 * @brief 音频服务初始化配置。
 */
typedef struct {
    uint8_t volume;              /**< 播放音量，范围 0-100；填 0 使用默认值 100。 */
    uint8_t passthrough_gain;    /**< 本地直通软件增益；填 0 使用默认值 1。 */
    service_audio_input_t input; /**< 默认单声道输入来源。 */
} service_audio_config_t;

/**
 * @brief 初始化音频服务。
 *
 * @param[in] cfg 初始化配置，可为 NULL。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_init(const service_audio_config_t *cfg);

/**
 * @brief 释放音频服务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_deinit(void);

/**
 * @brief 读取单声道 PCM 音频数据。
 *
 * @param[out] pcm 单声道 PCM 输出缓冲区。
 * @param[in] samples 期望读取的 int16_t 样本数。
 * @param[in] timeout_ms 单次底层读取超时时间，单位毫秒。
 * @return 实际读取的单声道样本数；失败返回负值。
 */
int service_audio_read(int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

/**
 * @brief 播放单声道 PCM 音频数据。
 *
 * 内部会把 mono PCM 复制到 L/R 两个 I2S slot，兼容耳机和单声道功放输出。
 *
 * @param[in] pcm 单声道 PCM 输入缓冲区。
 * @param[in] samples 待播放的 int16_t 样本数。
 * @param[in] timeout_ms 单次底层写入超时时间，单位毫秒。
 * @return 实际播放的单声道样本数；失败返回负值。
 */
int service_audio_play(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms);

/**
 * @brief 设置播放音量。
 *
 * @param[in] volume 音量百分比，范围 0-100。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_set_volume(uint8_t volume);

/**
 * @brief 设置播放静音状态。
 *
 * @param[in] mute 非 0 表示静音，0 表示取消静音。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_set_mute(int mute);

/**
 * @brief 设置单声道输入来源。
 *
 * @param[in] input 输入来源。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_set_input(service_audio_input_t input);

/**
 * @brief 设置本地直通软件增益。
 *
 * @param[in] gain 软件增益，填 0 等同于 1。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_set_passthrough_gain(uint8_t gain);

/**
 * @brief 执行一次本地直通调试。
 *
 * @param[in,out] pcm_buf 单声道 PCM 中间缓冲区。
 * @param[in] samples 中间缓冲区可容纳的 int16_t 样本数。
 * @param[in] timeout_ms 单次底层读写超时时间，单位毫秒。
 * @return 成功返回实际播放的单声道样本数；失败返回负值。
 */
int service_audio_passthrough_once(int16_t *pcm_buf,
                                   uint32_t samples,
                                   uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_AUDIO_H */
