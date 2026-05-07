/**
 * @file service_audio.c
 * @brief 音频能力服务实现。
 */
#include "service_audio.h"

#include "bsp_audio.h"
#include "bsp_es7210.h"
#include "bsp_es8311.h"
#include "bsp_i2c.h"
#include "bsp_i2s.h"
#include "osal_task.h"
#include "service_common.h"

#include <stddef.h>

static const char *TAG = "service_audio";

#define SERVICE_AUDIO_DEFAULT_VOLUME           100u
#define SERVICE_AUDIO_DEFAULT_GAIN             1u
#define SERVICE_AUDIO_CODEC_CLOCK_STABLE_MS    50u
#define SERVICE_AUDIO_CHUNK_FRAMES             256u
#define SERVICE_AUDIO_STEREO_SAMPLE_COUNT      (SERVICE_AUDIO_CHUNK_FRAMES * 2u)
#define SERVICE_AUDIO_STEREO_BYTES             (SERVICE_AUDIO_STEREO_SAMPLE_COUNT * sizeof(int16_t))

static es7210_handle_t s_es7210 = NULL;
static es8311_handle_t s_es8311 = NULL;
static service_audio_input_t s_input = SERVICE_AUDIO_INPUT_MIC1;
static uint8_t s_passthrough_gain = SERVICE_AUDIO_DEFAULT_GAIN;
static uint8_t s_stereo_read_buf[SERVICE_AUDIO_STEREO_BYTES];
static int16_t s_stereo_play_buf[SERVICE_AUDIO_STEREO_SAMPLE_COUNT];

static int service_audio_is_inited(void)
{
    return s_es7210 != NULL && s_es8311 != NULL;
}

static int service_audio_input_is_valid(service_audio_input_t input)
{
    return input == SERVICE_AUDIO_INPUT_MIC1 ||
           input == SERVICE_AUDIO_INPUT_MIC2 ||
           input == SERVICE_AUDIO_INPUT_MIX_AVG;
}

static int16_t service_audio_read_i16_le(const uint8_t *data, uint32_t sample_index)
{
    uint32_t offset = sample_index * 2u;
    uint16_t raw = (uint16_t)data[offset] | ((uint16_t)data[offset + 1u] << 8);
    return (int16_t)raw;
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

static int16_t service_audio_select_mono_sample(int16_t mic1, int16_t mic2)
{
    switch (s_input) {
    case SERVICE_AUDIO_INPUT_MIC2:
        return mic2;
    case SERVICE_AUDIO_INPUT_MIX_AVG:
        return (int16_t)(((int32_t)mic1 + (int32_t)mic2) / 2);
    case SERVICE_AUDIO_INPUT_MIC1:
    default:
        return mic1;
    }
}

static uint32_t service_audio_stereo_to_mono(const uint8_t *input,
                                             uint32_t input_bytes,
                                             int16_t *output,
                                             uint32_t output_samples)
{
    uint32_t frame_count = input_bytes / 4u;

    if (frame_count > output_samples) {
        frame_count = output_samples;
    }

    for (uint32_t i = 0; i < frame_count; i++) {
        int16_t mic1 = service_audio_read_i16_le(input, i * 2u);
        int16_t mic2 = service_audio_read_i16_le(input, i * 2u + 1u);
        output[i] = service_audio_select_mono_sample(mic1, mic2);
    }

    return frame_count;
}

static uint32_t service_audio_mono_to_stereo(const int16_t *input,
                                             uint32_t input_samples,
                                             int16_t *output,
                                             uint32_t output_samples)
{
    uint32_t frame_count = input_samples;
    uint32_t max_frames = output_samples / 2u;

    if (frame_count > max_frames) {
        frame_count = max_frames;
    }

    for (uint32_t i = 0; i < frame_count; i++) {
        output[i * 2u] = input[i];
        output[i * 2u + 1u] = input[i];
    }

    return frame_count;
}

int service_audio_init(const service_audio_config_t *cfg)
{
    if (service_audio_is_inited()) {
        SERVICE_LOGI(TAG, "音频服务已初始化");
        return 0;
    }

    uint8_t volume = SERVICE_AUDIO_DEFAULT_VOLUME;
    s_passthrough_gain = SERVICE_AUDIO_DEFAULT_GAIN;
    s_input = SERVICE_AUDIO_INPUT_MIC1;

    if (cfg != NULL) {
        volume = cfg->volume == 0u ? SERVICE_AUDIO_DEFAULT_VOLUME : cfg->volume;
        s_passthrough_gain = cfg->passthrough_gain == 0u ?
                             SERVICE_AUDIO_DEFAULT_GAIN :
                             cfg->passthrough_gain;
        s_input = cfg->input;
    }

    if (!service_audio_input_is_valid(s_input)) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 输入来源无效, input=%d", s_input);
        return -1;
    }

    int ret = bsp_audio_codec_power_on();
    if (ret != 0) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: codec 使能失败, ret=%d", ret);
        return ret;
    }

    osal_delay_ms(SERVICE_AUDIO_CODEC_CLOCK_STABLE_MS);

    es7210_interface_t es7210_itf = {
        .write_reg = es7210_i2c_write_reg_impl,
        .read_reg = es7210_i2c_read_reg_impl,
        .read = es7210_i2s_read_impl,
    };
    es8311_interface_t es8311_itf = {
        .write_reg = es8311_i2c_write_reg_impl,
        .read_reg = es8311_i2c_read_reg_impl,
        .write = es8311_i2s_write_impl,
    };

    s_es7210 = es7210_init(&es7210_itf);
    if (s_es7210 == NULL) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: ES7210 初始化失败");
        return -2;
    }

    s_es8311 = es8311_init(&es8311_itf);
    if (s_es8311 == NULL) {
        es7210_deinit(s_es7210);
        s_es7210 = NULL;
        SERVICE_LOGE(TAG, "音频服务初始化失败: ES8311 初始化失败");
        return -3;
    }

    ret = es8311_set_volume(s_es8311, volume);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 设置音量失败, ret=%d", ret);
        service_audio_deinit();
        return ret;
    }

    ret = es8311_set_mute(s_es8311, 0);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "音频服务初始化失败: 取消静音失败, ret=%d", ret);
        service_audio_deinit();
        return ret;
    }

    SERVICE_LOGI(TAG, "音频服务初始化成功, volume=%u, input=%d, gain=%u",
                 (unsigned int)volume,
                 s_input,
                 (unsigned int)s_passthrough_gain);
    return 0;
}

int service_audio_deinit(void)
{
    if (s_es8311 != NULL) {
        es8311_deinit(s_es8311);
        s_es8311 = NULL;
    }

    if (s_es7210 != NULL) {
        es7210_deinit(s_es7210);
        s_es7210 = NULL;
    }

    (void)bsp_audio_codec_power_off();
    SERVICE_LOGI(TAG, "音频服务已释放");
    return 0;
}

int service_audio_read(int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es7210 == NULL) {
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
        uint32_t chunk_frames = remain_samples > SERVICE_AUDIO_CHUNK_FRAMES ?
                                SERVICE_AUDIO_CHUNK_FRAMES :
                                remain_samples;
        uint32_t read_len = chunk_frames * 4u;
        int read_bytes = es7210_read(s_es7210, s_stereo_read_buf, read_len, timeout_ms);

        if (read_bytes < 0) {
            return read_bytes;
        }
        if (read_bytes == 0) {
            break;
        }

        uint32_t converted = service_audio_stereo_to_mono(s_stereo_read_buf,
                                                          (uint32_t)read_bytes,
                                                          &pcm[total_samples],
                                                          remain_samples);
        if (converted == 0u) {
            break;
        }

        total_samples += converted;

        if ((uint32_t)read_bytes < read_len) {
            break;
        }
    }

    return total_samples > 0u ? (int)total_samples : -3;
}

int service_audio_play(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms)
{
    if (s_es8311 == NULL) {
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
        uint32_t chunk_samples = remain_samples > SERVICE_AUDIO_CHUNK_FRAMES ?
                                 SERVICE_AUDIO_CHUNK_FRAMES :
                                 remain_samples;
        uint32_t frames = service_audio_mono_to_stereo(&pcm[total_samples],
                                                       chunk_samples,
                                                       s_stereo_play_buf,
                                                       SERVICE_AUDIO_STEREO_SAMPLE_COUNT);
        uint32_t write_len = frames * 4u;
        int written = es8311_play(s_es8311, (const uint8_t *)s_stereo_play_buf, write_len, timeout_ms);

        if (written < 0) {
            return written;
        }
        if (written == 0) {
            break;
        }

        uint32_t played = (uint32_t)written / 4u;
        total_samples += played;

        if ((uint32_t)written < write_len) {
            break;
        }
    }

    return total_samples > 0u ? (int)total_samples : -3;
}

int service_audio_set_volume(uint8_t volume)
{
    if (s_es8311 == NULL) {
        SERVICE_LOGE(TAG, "设置音量失败: 服务未初始化");
        return -1;
    }

    return es8311_set_volume(s_es8311, volume);
}

int service_audio_set_mute(int mute)
{
    if (s_es8311 == NULL) {
        SERVICE_LOGE(TAG, "设置静音失败: 服务未初始化");
        return -1;
    }

    return es8311_set_mute(s_es8311, mute);
}

int service_audio_set_input(service_audio_input_t input)
{
    if (!service_audio_input_is_valid(input)) {
        SERVICE_LOGE(TAG, "设置输入来源失败: input=%d", input);
        return -1;
    }

    s_input = input;
    SERVICE_LOGI(TAG, "音频输入来源已设置, input=%d", s_input);
    return 0;
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
