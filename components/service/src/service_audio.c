/**
 * @file service_audio.c
 * @brief 音频能力服务实现。
 */
#include "service_audio.h"

#include "service_config.h"

#include <stddef.h>

static const char *TAG = "service_audio";

#define SERVICE_AUDIO_DEFAULT_VOLUME           100u
#define SERVICE_AUDIO_DEFAULT_GAIN             1u
#define SERVICE_AUDIO_CHUNK_SAMPLES            256u

static service_audio_capture_ops_t s_capture_ops;
static service_audio_playback_ops_t s_playback_ops;
static uint8_t s_audio_inited = 0u;
static uint8_t s_passthrough_gain = SERVICE_AUDIO_DEFAULT_GAIN;

static int service_audio_is_inited(void)
{
    return s_audio_inited != 0u;
}

static int16_t service_audio_clip_i16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }

    return (int16_t)value;
}

static int service_audio_ops_are_valid(const service_audio_capture_ops_t *capture_ops,
                                       const service_audio_playback_ops_t *playback_ops)
{
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
    s_passthrough_gain = SERVICE_AUDIO_DEFAULT_GAIN;

    if (cfg != NULL) {
        if (service_audio_ops_are_valid(&cfg->capture_ops, &cfg->playback_ops) != 0) {
            SERVICE_LOGE(TAG, "音频服务初始化失败: ops 无效");
            return -5;
        }
        s_capture_ops = cfg->capture_ops;
        s_playback_ops = cfg->playback_ops;
        volume = cfg->volume == 0u ? SERVICE_AUDIO_DEFAULT_VOLUME : cfg->volume;
        s_passthrough_gain = cfg->passthrough_gain == 0u ?
                             SERVICE_AUDIO_DEFAULT_GAIN :
                             cfg->passthrough_gain;
    }

    if (s_capture_ops.read_pcm == NULL || s_playback_ops.play_pcm == NULL) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 未绑定音频能力");
        return -4;
    }

    if (s_capture_ops.is_initialized() != 1 || s_playback_ops.is_initialized() != 1) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 下层音频 driver 未初始化");
        s_capture_ops = (service_audio_capture_ops_t){0};
        s_playback_ops = (service_audio_playback_ops_t){0};
        return -6;
    }

    if (s_playback_ops.set_volume(volume) != 0) {
        SERVICE_LOGW(TAG, "音频服务初始化: 默认音量设置失败");
    }

    s_audio_inited = 1u;
    SERVICE_LOGI(TAG, "音频服务初始化成功, volume=%u, gain=%u",
                 (unsigned int)volume,
                 (unsigned int)s_passthrough_gain);
    return 0;
}

int service_audio_deinit(void)
{
    s_audio_inited = 0u;
    s_capture_ops = (service_audio_capture_ops_t){0};
    s_playback_ops = (service_audio_playback_ops_t){0};
    SERVICE_LOGI(TAG, "音频服务已释放");
    return 0;
}

int service_audio_start_record(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    return s_capture_ops.start_record != NULL ? s_capture_ops.start_record() : 0;
}

int service_audio_stop_record(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    return s_capture_ops.stop_record != NULL ? s_capture_ops.stop_record() : 0;
}

int service_audio_start_playback(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    return s_playback_ops.start_playback != NULL ? s_playback_ops.start_playback() : 0;
}

int service_audio_stop_playback(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    return s_playback_ops.stop_playback != NULL ? s_playback_ops.stop_playback() : 0;
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
    if (pcm == NULL || samples == 0u) {
        SERVICE_LOGE(TAG, "音频播放参数无效, pcm=%p, samples=%u",
                     pcm, (unsigned int)samples);
        return -2;
    }

    uint32_t total_samples = 0;
    while (total_samples < samples) {
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

int service_audio_set_passthrough_gain(uint8_t gain)
{
    s_passthrough_gain = gain == 0u ? SERVICE_AUDIO_DEFAULT_GAIN : gain;
    SERVICE_LOGI(TAG, "音频直通增益已设置, gain=%u", (unsigned int)s_passthrough_gain);
    return 0;
}

int service_audio_passthrough_once(int16_t *pcm_buf,
                                   uint32_t samples,
                                   uint32_t timeout_ms)
{
    if (!service_audio_is_inited()) {
        SERVICE_LOGE(TAG, "本地直通失败: 服务未初始化");
        return -1;
    }
    if (pcm_buf == NULL || samples == 0u) {
        SERVICE_LOGE(TAG, "本地直通参数无效, pcm_buf=%p, samples=%u",
                     pcm_buf, (unsigned int)samples);
        return -2;
    }

    int read_samples = service_audio_read(pcm_buf, samples, timeout_ms);
    if (read_samples <= 0) {
        SERVICE_LOGE(TAG, "本地直通读取失败, ret=%d", read_samples);
        return read_samples;
    }

    for (int i = 0; i < read_samples; i++) {
        pcm_buf[i] = service_audio_clip_i16((int32_t)pcm_buf[i] * s_passthrough_gain);
    }

    return service_audio_play(pcm_buf, (uint32_t)read_samples, timeout_ms);
}
