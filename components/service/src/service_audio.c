/**
 * @file service_audio.c
 * @brief 音频能力服务实现。
 *
 * 下层 capture driver 只提供 PCM 读取能力，下层 playback driver 只提供
 * PCM 播放、音量和静音能力。service_audio 在此基础上维护录音会话、
 * 录音缓存、流式读写和本地直通测试接口。
 */
#include "service_audio.h"

#include "service_config.h"
#include "osal_mutex.h"
#include "osal_task.h"

#include "esp_heap_caps.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "service_audio";

/** @brief 默认播放音量，范围 0-100。 */
#define SERVICE_AUDIO_DEFAULT_VOLUME           100u
/** @brief 本地直通默认软件增益。 */
#define SERVICE_AUDIO_DEFAULT_GAIN             1u
/** @brief service 层分块读写样本数，避免一次调用占用过长时间。 */
#define SERVICE_AUDIO_CHUNK_SAMPLES            256u
/** @brief AI 录音固定采样率，需与业务 WAV 配置一致。 */
#define SERVICE_AUDIO_RECORD_SAMPLE_RATE_HZ    16000u
/** @brief 单次 AI 录音最大时长。 */
#define SERVICE_AUDIO_RECORD_MAX_SECONDS       2u
/** @brief 录音任务每次从下层读取 20ms PCM。 */
#define SERVICE_AUDIO_RECORD_FRAME_SAMPLES     320u
/** @brief 单次录音最大样本数。 */
#define SERVICE_AUDIO_RECORD_MAX_SAMPLES       (SERVICE_AUDIO_RECORD_SAMPLE_RATE_HZ * SERVICE_AUDIO_RECORD_MAX_SECONDS)
/** @brief 录音任务栈大小。 */
#define SERVICE_AUDIO_RECORD_TASK_STACK        3072u
/** @brief 录音任务优先级。 */
#define SERVICE_AUDIO_RECORD_TASK_PRIORITY     5u
/** @brief 录音任务单帧读取超时。 */
#define SERVICE_AUDIO_RECORD_READ_TIMEOUT_MS   30u
/** @brief stop_record 等待录音任务退出当前帧读取的最长时间。 */
#define SERVICE_AUDIO_RECORD_STOP_WAIT_MS      150u

/** @brief 当前绑定的采集 driver 能力函数表。 */
static service_audio_capture_ops_t s_capture_ops;
/** @brief 当前绑定的播放 driver 能力函数表。 */
static service_audio_playback_ops_t s_playback_ops;
/** @brief 音频 service 是否已初始化完成。 */
static uint8_t s_audio_inited = 0u;
/** @brief 本地直通调试用软件增益，只影响 passthrough_once。 */
static uint8_t s_passthrough_gain = SERVICE_AUDIO_DEFAULT_GAIN;
/** @brief 当前是否处于 service 录音会话中。 */
static uint8_t s_recording = 0u;
/** @brief 录音任务是否正在执行采集循环。 */
static uint8_t s_record_task_active = 0u;
/** @brief 播放会话标志，play() 当前要求 start_playback 后才能播放。 */
static uint8_t s_playback_started = 0u;
/** @brief 录音后台任务句柄。 */
static osal_task_t s_record_task = NULL;
/** @brief 保护录音缓存长度和内容的互斥锁。 */
static osal_mutex_t s_record_mutex = NULL;
/** @brief AI 录音缓存，分配在 PSRAM。 */
static int16_t *s_record_buf = NULL;
/** @brief 当前录音缓存中有效样本数。 */
static uint32_t s_record_samples = 0u;

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
 * @brief 将 32 位整数样本饱和裁剪到 int16_t 范围。
 *
 * @param[in] value 待裁剪的样本值。
 * @return 裁剪后的 int16_t 样本。
 */
static int16_t service_audio_clip_i16(int32_t value)
{
    /* 软件增益可能溢出 int16_t，写入播放前需要饱和裁剪。 */
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
    /* service 录音和播放路径都依赖这些能力，初始化阶段一次性校验。 */
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

static int service_audio_alloc_record_buffer(void)
{
    if (s_record_buf != NULL) {
        return 0;
    }

    size_t bytes = SERVICE_AUDIO_RECORD_MAX_SAMPLES * sizeof(int16_t);
    /*
     * 2 秒 16kHz/16bit/mono 录音约 64KB，放 PSRAM 避免占用内部 SRAM，
     * 给 OSAL task stack、WiFi/LVGL 等内部内存留空间。
     */
    s_record_buf = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_record_buf == NULL) {
        SERVICE_LOGE(TAG,
                     "录音缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                     (unsigned int)bytes,
                     (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return -1;
    }

    SERVICE_LOGI(TAG,
                 "录音缓存已分配到 PSRAM, bytes=%u",
                 (unsigned int)bytes);
    return 0;
}

static void service_audio_record_task(void *arg)
{
    (void)arg;
    int16_t frame[SERVICE_AUDIO_RECORD_FRAME_SAMPLES];

    while (1) {
        /* start_record() 通过 notify 唤醒任务；空闲时不轮询、不占 CPU。 */
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        s_record_task_active = 1u;

        while (s_recording != 0u) {
            uint32_t room = 0u;
            if (osal_mutex_lock(s_record_mutex, OSAL_WAIT_FOREVER) == 0) {
                room = SERVICE_AUDIO_RECORD_MAX_SAMPLES - s_record_samples;
                osal_mutex_unlock(s_record_mutex);
            }
            if (room == 0u) {
                /* 缓冲区写满后自动停止，AI 单次录音最长 2 秒。 */
                s_recording = 0u;
                break;
            }

            uint32_t request = room > SERVICE_AUDIO_RECORD_FRAME_SAMPLES ?
                               SERVICE_AUDIO_RECORD_FRAME_SAMPLES :
                               room;
            int read_samples = service_audio_read(frame,
                                                  request,
                                                  SERVICE_AUDIO_RECORD_READ_TIMEOUT_MS);
            if (read_samples <= 0) {
                /* 下层短暂无数据时稍等再读，不把一次超时视为录音失败。 */
                osal_delay_ms(5u);
                continue;
            }

            if (osal_mutex_lock(s_record_mutex, OSAL_WAIT_FOREVER) == 0) {
                uint32_t writable = SERVICE_AUDIO_RECORD_MAX_SAMPLES - s_record_samples;
                if ((uint32_t)read_samples > writable) {
                    read_samples = (int)writable;
                }
                if (read_samples > 0) {
                    memcpy(&s_record_buf[s_record_samples],
                           frame,
                           (uint32_t)read_samples * sizeof(int16_t));
                    s_record_samples += (uint32_t)read_samples;
                }
                osal_mutex_unlock(s_record_mutex);
            }
        }

        s_record_task_active = 0u;
    }
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
        /* cfg 中的 ops 会被复制到静态变量，调用方可使用局部临时配置。 */
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
        /* 严格要求 driver 先初始化，service 不负责初始化具体硬件。 */
        SERVICE_LOGE(TAG, "音频服务初始化失败: 下层音频 driver 未初始化");
        s_capture_ops = (service_audio_capture_ops_t){0};
        s_playback_ops = (service_audio_playback_ops_t){0};
        return -6;
    }

    if (service_audio_alloc_record_buffer() != 0) {
        return -7;
    }

    if (s_record_mutex == NULL) {
        s_record_mutex = osal_mutex_create();
        if (s_record_mutex == NULL) {
            SERVICE_LOGE(TAG, "音频服务初始化失败: 录音互斥锁创建失败");
            return -8;
        }
    }

    if (s_record_task == NULL) {
        if (osal_task_create("svc_audio_rec",
                             service_audio_record_task,
                             NULL,
                             SERVICE_AUDIO_RECORD_TASK_STACK,
                             SERVICE_AUDIO_RECORD_TASK_PRIORITY,
                             &s_record_task) != 0) {
            SERVICE_LOGE(TAG, "音频服务初始化失败: 录音任务创建失败");
            return -9;
        }
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
    s_recording = 0u;
    s_record_task_active = 0u;
    s_playback_started = 0u;
    s_record_samples = 0u;
    s_capture_ops = (service_audio_capture_ops_t){0};
    s_playback_ops = (service_audio_playback_ops_t){0};
    if (s_record_buf != NULL) {
        heap_caps_free(s_record_buf);
        s_record_buf = NULL;
    }
    SERVICE_LOGI(TAG, "音频服务已释放");
    return 0;
}

int service_audio_start_record(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }
    if (s_record_mutex == NULL || s_record_task == NULL) {
        return -2;
    }

    if (osal_mutex_lock(s_record_mutex, OSAL_WAIT_FOREVER) != 0) {
        return -3;
    }
    /* 开始一次新录音前丢弃上一次数据。 */
    s_record_samples = 0u;
    s_recording = 1u;
    osal_mutex_unlock(s_record_mutex);

    return osal_task_notify_give(s_record_task);
}

int service_audio_stop_record(void)
{
    if (!service_audio_is_inited()) {
        return -1;
    }

    s_recording = 0u;
    uint32_t start = osal_get_tick_ms();
    /* 等待录音任务结束当前底层 read_pcm，避免马上读取到半更新的缓存长度。 */
    while (s_record_task_active != 0u &&
           (osal_get_tick_ms() - start) < SERVICE_AUDIO_RECORD_STOP_WAIT_MS) {
        osal_delay_ms(5u);
    }

    return s_record_task_active == 0u ? 0 : -2;
}

int service_audio_get_record_data(const int16_t **pcm, uint32_t *samples)
{
    if (!service_audio_is_inited() || pcm == NULL || samples == NULL ||
        s_record_mutex == NULL || s_record_buf == NULL) {
        return -1;
    }
    if (s_recording != 0u) {
        return -2;
    }
    if (s_record_task_active != 0u) {
        return -4;
    }
    if (osal_mutex_lock(s_record_mutex, OSAL_WAIT_FOREVER) != 0) {
        return -3;
    }

    *pcm = s_record_buf;
    *samples = s_record_samples;
    osal_mutex_unlock(s_record_mutex);
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
    uint8_t playback_was_started = s_playback_started;

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
        /* 直通测试才在 service 层做软件增益，正式音量控制仍由 playback driver 实现。 */
        pcm_buf[i] = service_audio_clip_i16((int32_t)pcm_buf[i] * s_passthrough_gain);
    }

    s_playback_started = 1u;
    int ret = service_audio_play(pcm_buf, (uint32_t)read_samples, timeout_ms);
    s_playback_started = playback_was_started;
    return ret;
}
