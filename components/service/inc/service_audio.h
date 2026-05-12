/**
 * @file service_audio.h
 * @brief 音频能力服务接口。
 *
 * 本服务对上提供单声道 PCM 录放能力。底层采集和播放设备通过能力接口绑定，
 * service 负责录放状态、参数检查和业务友好的录放 API。
 */
#ifndef SERVICE_AUDIO_H
#define SERVICE_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频服务依赖的下层采集能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层采集 driver 是否已初始化。 */
    int (*read_pcm)(int16_t *pcm,
                    uint32_t samples,
                    uint32_t timeout_ms); /**< 读取单声道 PCM 样本。 */
} service_audio_capture_ops_t;

/**
 * @brief 音频服务依赖的下层播放能力。
 */
typedef struct {
    int (*is_initialized)(void); /**< 判断下层播放 driver 是否已初始化。 */
    int (*play_pcm)(const int16_t *pcm,
                    uint32_t samples,
                    uint32_t timeout_ms); /**< 播放单声道 PCM 样本。 */
    int (*set_volume)(uint8_t volume);    /**< 设置下层播放音量。 */
    int (*set_mute)(int mute);            /**< 设置下层静音状态。 */
} service_audio_playback_ops_t;

/**
 * @brief 音频服务初始化配置。
 */
typedef struct {
    service_audio_capture_ops_t capture_ops;   /**< 下层采集能力函数表。 */
    service_audio_playback_ops_t playback_ops; /**< 下层播放能力函数表。 */
    uint8_t volume;              /**< 播放音量，范围 0-100；填 0 使用默认值 100。 */
    uint8_t passthrough_gain;    /**< 本地直通软件增益；填 0 使用默认值 1。 */
} service_audio_config_t;

/**
 * @brief 初始化音频服务。
 *
 * 初始化时会复制 cfg 中的采集/播放 ops，后续 service_audio API 只调用
 * 自己保存的函数表，不直接依赖具体 driver。
 *
 * @param[in] cfg 初始化配置；为 NULL 时使用已绑定状态检查初始化情况。
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
 * @brief 开始录音。
 *
 * 清空内部录音缓冲区并唤醒录音任务。录音数据会被写入 service 内部 PSRAM
 * 缓冲区，直到 stop_record 或达到最大录音时长。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_start_record(void);

/**
 * @brief 停止录音。
 *
 * 通知录音任务退出采集循环，并等待当前帧读取结束。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_stop_record(void);

/**
 * @brief 获取最近一次录音 PCM 数据。
 *
 * 指针指向 service 内部静态缓冲区，在下一次开始录音前有效。
 *
 * @param[out] pcm 录音 PCM 指针。
 * @param[out] samples 录音样本数。
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_get_record_data(const int16_t **pcm, uint32_t *samples);

/**
 * @brief 开始播放。
 *
 * 当前实现只维护播放会话状态，实际硬件播放由 service_audio_play() 转发到
 * playback driver。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_start_playback(void);

/**
 * @brief 停止播放。
 *
 * @return 成功返回 0；失败返回负值。
 */
int service_audio_stop_playback(void);

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
 * 下层播放 driver 负责把业务单声道 PCM 适配到具体硬件帧格式。
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
