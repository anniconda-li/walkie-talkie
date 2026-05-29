/**
 * @file service_audio.c
 * @brief 音频能力服务实现。
 *
 * 下层 capture driver 只提供 PCM 读取能力，下层 playback driver 只提供
 * PCM 播放、音量和静音能力。service_audio 在此基础上维护播放状态和
 * 分块读写，不持有 AI、对讲等业务的整段音频缓存。
 */
#include "service_audio.h"

#include "service_config.h"

#include <stddef.h>

/** @brief 音频 service 日志标签。 */
static const char *TAG = "service_audio";

/** @brief 默认播放音量，范围 0-100。 */
#define SERVICE_AUDIO_DEFAULT_VOLUME           100u
/** @brief service 层分块读写样本数，避免一次调用占用过长时间。 */
#define SERVICE_AUDIO_CHUNK_SAMPLES            256u

/** @brief 当前绑定的采集 driver 能力函数表。 */
static service_audio_capture_ops_t s_capture_ops;
/** @brief 当前绑定的播放 driver 能力函数表。 */
static service_audio_playback_ops_t s_playback_ops;
/** @brief 音频 service 是否已初始化完成。 */
static uint8_t s_audio_inited = 0u;
/** @brief 播放会话标志，play() 当前要求 start_playback 后才能播放。 */
static uint8_t s_playback_started = 0u;

/**
 * @brief 查询音频 service 是否已经完成初始化。
 *
 * @return 已初始化返回非 0；未初始化返回 0。
 */
static int service_audio_is_inited(void)
{
    return s_audio_inited != 0u;
}

/**
 * @brief 校验音频 service 依赖的底层能力函数表是否完整。
 *
 * service_audio 不直接依赖具体设备文件，只通过初始化时绑定的 ops 调用底层
 * capture/playback 能力。初始化阶段集中校验，避免运行时空函数指针。
 *
 * @param[in] capture_ops 采集能力函数表。
 * @param[in] playback_ops 播放能力函数表。
 * @return 完整返回 0；缺少必要函数返回 -1。
 */
static int service_audio_ops_are_valid(const service_audio_capture_ops_t *capture_ops,
                                       const service_audio_playback_ops_t *playback_ops)
{
    /* service 读 PCM 和播 PCM 路径都依赖这些能力，初始化阶段一次性校验。 */
    if (capture_ops == NULL ||
        capture_ops->is_initialized == NULL ||
        capture_ops->read_pcm == NULL ||
        playback_ops == NULL ||
        playback_ops->is_initialized == NULL ||
        playback_ops->play_pcm == NULL ||
        playback_ops->set_volume == NULL ||
        playback_ops->set_mute == NULL) {
        return -1;
    }

    return 0;
}

int service_audio_init(const service_audio_config_t *cfg)
{
    if (service_audio_is_inited()) {
        SERVICE_LOGI(TAG, "音频服务已初始化");
        return 0;
    }

    uint8_t volume = SERVICE_AUDIO_DEFAULT_VOLUME;

    if (cfg != NULL) {
        /* cfg 中的 ops 会被复制到静态变量，调用方可使用局部临时配置。 */
        if (service_audio_ops_are_valid(&cfg->capture_ops, &cfg->playback_ops) != 0) {
            SERVICE_LOGE(TAG, "音频服务初始化失败: ops 无效");
            return -5;
        }
        s_capture_ops = cfg->capture_ops;
        s_playback_ops = cfg->playback_ops;
        volume = cfg->volume == 0u ? SERVICE_AUDIO_DEFAULT_VOLUME : cfg->volume;
    }

    if (s_capture_ops.read_pcm == NULL || s_playback_ops.play_pcm == NULL) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 未绑定音频能力");
        return -4;
    }

    if (s_capture_ops.is_initialized() != 1 || s_playback_ops.is_initialized() != 1) {
        /* 严格要求 driver 先初始化，service 不负责初始化具体硬件。 */
        SERVICE_LOGE(TAG, "音频服务初始化失败: 下层音频 driver 未初始化");
        s_capture_ops = (service_audio_capture_ops_t){0};
        s_playback_ops = (service_audio_playback_ops_t){0};
        return -6;
    }

    if (s_playback_ops.set_volume(volume) != 0) {
        SERVICE_LOGW(TAG, "音频服务初始化: 默认音量设置失败");
    }

    s_audio_inited = 1u;
    SERVICE_LOGI(TAG, "音频服务初始化成功, volume=%u", (unsigned int)volume);
    return 0;
}

int service_audio_deinit(void)
{
    s_audio_inited = 0u;
    s_playback_started = 0u;
    s_capture_ops = (service_audio_capture_ops_t){0};
    s_playback_ops = (service_audio_playback_ops_t){0};
    SERVICE_LOGI(TAG, "音频服务已释放");
    return 0;
}

int service_audio_start_playback(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    s_playback_started = 1u;
    return 0;
}

int service_audio_stop_playback(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    s_playback_started = 0u;
    return 0;
}

int service_audio_read(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (!service_audio_is_inited() || s_capture_ops.read_pcm == NULL) {
        SERVICE_LOGE(TAG, "音频读取失败: 服务未初始化");
        return -1;
    }
    if (pcm == NULL || samples == 0u) {
        SERVICE_LOGE(TAG, "音频读取参数无效, pcm=%p, samples=%u",
                     pcm, (unsigned int)samples);
        return -2;
    }

    uint32_t total_samples = 0;
    while (total_samples < samples) {
        /* 分块读取，避免请求大块 PCM 时单次底层调用阻塞太久。 */
        uint32_t remain_samples = samples - total_samples;
        uint32_t chunk_frames = remain_samples > SERVICE_AUDIO_CHUNK_SAMPLES ?
                                SERVICE_AUDIO_CHUNK_SAMPLES :
                                remain_samples;
        int read_samples = s_capture_ops.read_pcm(&pcm[total_samples], chunk_frames, timeout_ms);

        if (read_samples < 0) {
            return read_samples;
        }
        if (read_samples == 0) {
            break;
        }

        total_samples += (uint32_t)read_samples;

        if ((uint32_t)read_samples < chunk_frames) {
            break;
        }
    }

    return total_samples > 0u ? (int)total_samples : -3;
}

int service_audio_play(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (!service_audio_is_inited() || s_playback_ops.play_pcm == NULL) {
        SERVICE_LOGE(TAG, "音频播放失败: 服务未初始化");
        return -1;
    }
    if (s_playback_started == 0u) {
        SERVICE_LOGE(TAG, "音频播放失败: 播放未开始");
        return -4;
    }
    if (pcm == NULL || samples == 0u) {
        SERVICE_LOGE(TAG, "音频播放参数无效, pcm=%p, samples=%u",
                     pcm, (unsigned int)samples);
        return -2;
    }

    uint32_t total_samples = 0;
    while (total_samples < samples) {
        /* 分块播放，兼容底层 I2S/codec 一次只能写入部分样本的情况。 */
        uint32_t remain_samples = samples - total_samples;
        uint32_t chunk_samples = remain_samples > SERVICE_AUDIO_CHUNK_SAMPLES ?
                                 SERVICE_AUDIO_CHUNK_SAMPLES :
                                 remain_samples;
        int written = s_playback_ops.play_pcm(&pcm[total_samples], chunk_samples, timeout_ms);

        if (written < 0) {
            return written;
        }
        if (written == 0) {
            break;
        }

        total_samples += (uint32_t)written;

        if ((uint32_t)written < chunk_samples) {
            break;
        }
    }

    return total_samples > 0u ? (int)total_samples : -3;
}

int service_audio_set_volume(uint8_t volume)
{
    if (!service_audio_is_inited() || s_playback_ops.set_volume == NULL) {
        SERVICE_LOGE(TAG, "设置音量失败: 服务未初始化");
        return -1;
    }

    return s_playback_ops.set_volume(volume);
}

int service_audio_set_mute(int mute)
{
    if (!service_audio_is_inited() || s_playback_ops.set_mute == NULL) {
        SERVICE_LOGE(TAG, "设置静音失败: 服务未初始化");
        return -1;
    }

    return s_playback_ops.set_mute(mute);
}
