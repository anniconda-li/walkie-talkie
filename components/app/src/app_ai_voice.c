/**
 * @file app_ai_voice.c
 * @brief AI 语音问答业务——录音→上传→播放全流程。
 *
 * ## 业务流程概述
 * 1. 用户长按 UI 上的 AI 按钮 → UI 回调触发 app_ai_voice_record_start()
 * 2. 抢占音频会话互斥锁（与 PTT 互斥）→ 唤醒 AI 任务采集 PCM
 * 3. AI 任务逐个 20ms 帧编码为裸 Opus packet，写入 AOP1 缓冲区
 * 4. 用户松手 → UI 回调触发 app_ai_voice_record_stop()
 * 5. 停止录音 → biz_ai 任务退出采集循环
 * 6. biz_ai 任务：回填 AOP1 文件头 → WAI1 WebSocket 停等上传
 * 7. 收齐并校验 ROP1/Opus 回复 → 用户点击后完全离线解码播放
 * 8. 释放音频会话互斥锁，等待下一次唤醒
 *
 * ## 任务调度关系
 * - UI 线程（LVGL）→ 调用 record_start/stop → 设置标志位 + notify 目标任务
 * - biz_ai（优先级5）→ 等待 notify，采集录音、提交 WebSocket 请求和回答播放
 */
#include "app_ai_voice.h"

#include "app_ai_ws.h"
#include "app_audio_opus.h"
#include "app_business.h"
#include "app_config.h"
#include "app_ui.h"
#include "osal_heap.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_opus_dec.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ai_voice";
static const char *CANCEL_TAG = "APP_AI_CANCEL";

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief AI 处理任务句柄，入口为 app_ai_voice_task()，优先级 5，栈 32768 字节。 */
static osal_task_t s_ai_task = NULL;

/** @brief AI 模块是否已通过 app_ai_voice_start() 启动。
 *  启动后 task 和 WAV buffer 都已就绪，可以响应录音请求。 */
static volatile int s_started = 0;

/** @brief 当前是否处于 AI 录音进行中。
 *  由 record_start 置 1，record_stop 清 0。
 *  record_stop 会检查此标志，防止重复停止。 */
static volatile int s_ai_recording = 0;

/** @brief AI 请求 AOP1 缓冲区，分配到 PSRAM，保存 60 秒内的裸 Opus packets。 */
static uint8_t *s_ai_request_audio_buf = NULL;

/** @brief AI 回复 ROP1/Opus 缓冲区，完整下载并校验后才允许播放。 */
static uint8_t *s_ai_reply_chunk_buf = NULL;

/** @brief AI 分片协议 JSON 响应临时缓冲，避免覆盖正在上传的 AOP1 数据。 */
static uint8_t s_ai_resp_buf[4096];

typedef struct {
    char session[64];
    uint32_t total;
    char answer_text[1024];
    char status[24];
    char tts_status[24];
    char tts_error[128];
    uint8_t text_ready;
    uint8_t audio_ready;
    uint8_t audio_failed;
    uint8_t no_speech;
    uint8_t canceled;
    uint8_t failed;
} app_ai_voice_result_info_t;

static app_ai_voice_result_info_t s_ai_result_info;
static char s_reply_session[64];
static uint32_t s_reply_wav_size = 0u;
static volatile int s_reply_audio_ready = 0;
static volatile int s_reply_play_busy = 0;
static volatile int s_reply_stop_requested = 0;
static uint32_t s_ai_request_sequence = 0u;

typedef enum {
    APP_AI_STATE_IDLE = 0,
    APP_AI_STATE_RECORDING,
    APP_AI_STATE_UPLOADING,
    APP_AI_STATE_FINISHING,
    APP_AI_STATE_WAITING_TEXT,
    APP_AI_STATE_WAITING_AUDIO,
    APP_AI_STATE_AUDIO_READY,
    APP_AI_STATE_DOWNLOADING_AUDIO,
    APP_AI_STATE_PLAYING_AUDIO,
    APP_AI_STATE_CANCELING,
    APP_AI_STATE_CANCELED,
    APP_AI_STATE_FAILED,
} app_ai_voice_state_t;

static volatile int s_ai_cancel_requested = 0;
static volatile app_ai_voice_state_t s_ai_state = APP_AI_STATE_IDLE;
static char s_ai_current_session[64];

#define APP_AI_CANCELED_RET                 (-900)
#define APP_AI_PLAY_STOPPED_RET             (-901)
#define APP_AI_TEXT_ONLY_RET                (-902)
#define APP_AI_BACKEND_CANCEL_ON_DEVICE     0

#define APP_AI_AUDIO_FORMAT                 "opus_packets_v1"
#define APP_AI_AOP1_HEADER_LEN              24u
#define APP_AI_AOP1_VERSION                 1u
#define APP_AI_AOP1_FRAME_DURATION_MS       20u
#define APP_AI_AOP1_PACKET_LEN_BYTES        2u
#define APP_AI_AOP1_MAX_FRAMES              3000u
#define APP_AI_TASK_STACK                   32768u
#define APP_AI_REPLY_PLAY_TASK_STACK        32768u
/** 20 kbps CBR 的每个 20 ms packet 为 50 字节，60 秒 AOP1 共 156024 字节。 */
#define APP_AI_OPUS_CBR_PACKET_BYTES \
    ((APP_INTERCOM_OPUS_BITRATE * APP_AI_AOP1_FRAME_DURATION_MS + 7999u) / 8000u)
#define APP_AI_OPUS_CBR_MAX_CONTAINER_BYTES \
    (APP_AI_AOP1_HEADER_LEN + \
     APP_AI_AOP1_MAX_FRAMES * (APP_AI_AOP1_PACKET_LEN_BYTES + APP_AI_OPUS_CBR_PACKET_BYTES))

_Static_assert(APP_BUSINESS_AUDIO_SAMPLE_RATE == 16000u, "AOP1 sample rate must be 16 kHz");
_Static_assert(APP_BUSINESS_AUDIO_CHANNELS == 1u, "AOP1 must be mono");
_Static_assert(APP_BUSINESS_AUDIO_BITS == 16u, "Opus encoder input must be 16-bit PCM");
_Static_assert(APP_BUSINESS_FRAME_SAMPLES == APP_AUDIO_OPUS_FRAME_SAMPLES,
               "AI and intercom must share 20 ms frames");
_Static_assert(APP_BUSINESS_AI_MAX_SAMPLES == 960000u, "AI sample limit must be 60 seconds");
_Static_assert(APP_BUSINESS_AI_MAX_SAMPLES / APP_BUSINESS_FRAME_SAMPLES == APP_AI_AOP1_MAX_FRAMES,
               "AI frame limit must be 3000");
_Static_assert(APP_AI_UPLOAD_CHUNK_BYTES <= 65535u, "ML307C upload chunks must fit uint16");
_Static_assert(APP_AI_OPUS_CBR_MAX_CONTAINER_BYTES <= APP_BUSINESS_AI_OPUS_BUF_BYTES,
               "60-second CBR AOP1 must fit the request buffer");

#if APP_AI_BACKEND_CANCEL_ON_DEVICE
static volatile int s_ai_cancel_task_busy = 0;
static char s_ai_cancel_task_session[64];
#define APP_AI_CANCEL_HTTP_TIMEOUT_MS       5000u
#endif

static int app_ai_voice_is_cancel_requested(void)
{
    return s_ai_cancel_requested != 0;
}

static int app_ai_voice_is_playback_interrupted(void)
{
    return s_ai_cancel_requested != 0 || s_reply_stop_requested != 0;
}

static void app_ai_voice_set_state(app_ai_voice_state_t state)
{
    s_ai_state = state;
}

static void app_ai_voice_set_current_session(const char *session)
{
    if (session == NULL || session[0] == '\0') {
        s_ai_current_session[0] = '\0';
        return;
    }

    strncpy(s_ai_current_session, session, sizeof(s_ai_current_session) - 1u);
    s_ai_current_session[sizeof(s_ai_current_session) - 1u] = '\0';
}

static int app_ai_voice_is_active_session(const char *session)
{
    return session != NULL &&
           s_ai_current_session[0] != '\0' &&
           strcmp(session, s_ai_current_session) == 0;
}

static int app_ai_voice_is_audio_fetch_allowed(const char *session)
{
    if (!app_ai_voice_is_active_session(session) || app_ai_voice_is_playback_interrupted()) {
        return 0;
    }

    app_ai_voice_state_t state = s_ai_state;
    return state == APP_AI_STATE_DOWNLOADING_AUDIO || state == APP_AI_STATE_PLAYING_AUDIO;
}

static int app_ai_voice_can_start_recording_now(void)
{
    app_ai_voice_state_t state = s_ai_state;
    return state == APP_AI_STATE_IDLE ||
           state == APP_AI_STATE_AUDIO_READY ||
           state == APP_AI_STATE_CANCELED ||
           state == APP_AI_STATE_FAILED;
}

/* ==========================================================================
 * 小端字段与 AOP1 格式辅助函数
 * ========================================================================== */

/**
 * @brief 向缓冲区写入 little-endian uint16 字段。
 *
 * AOP1 和 WAV 文件格式要求所有多字节字段为小端序，
 * 这在 ESP32（小端 CPU）上等同于直接写入内存，但显式拆分保证跨平台正确性。
 */
static void app_ai_voice_write_u16_le(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

/** @brief 从缓冲区读取 little-endian uint16 字段。 */
static uint16_t app_ai_voice_wav_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/** @brief 向缓冲区写入 little-endian uint32 字段。 */
static void app_ai_voice_write_u32_le(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

/** @brief 从缓冲区读取 little-endian uint32 字段。 */
static uint32_t app_ai_voice_wav_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/** @brief 写入或回填固定 24 字节 AOP1 文件头。 */
static void app_ai_voice_aop1_write_header(uint8_t *buf,
                                            uint32_t frame_count,
                                            uint32_t pcm_samples)
{
    memcpy(&buf[0], "AOP1", 4u);
    buf[4] = APP_AI_AOP1_VERSION;
    buf[5] = APP_BUSINESS_AUDIO_CHANNELS;
    app_ai_voice_write_u16_le(&buf[6], APP_AI_AOP1_HEADER_LEN);
    app_ai_voice_write_u32_le(&buf[8], APP_BUSINESS_AUDIO_SAMPLE_RATE);
    app_ai_voice_write_u16_le(&buf[12], APP_BUSINESS_FRAME_SAMPLES);
    app_ai_voice_write_u16_le(&buf[14], APP_AI_AOP1_FRAME_DURATION_MS);
    app_ai_voice_write_u32_le(&buf[16], frame_count);
    app_ai_voice_write_u32_le(&buf[20], pcm_samples);
}

/** @brief WAV fmt chunk 中当前解析器至少需要的 PCM 格式字段长度。 */
#define APP_AI_WAV_FMT_MIN_BYTES      16u
/** @brief AI 回复播放时单次提交给 service_audio 的最大 PCM 样本数。 */
#define APP_AI_PLAY_CHUNK_SAMPLES     256u
/** @brief AI 回复下载和播放之间的 PCM 预缓冲块样本数。 */
#define APP_AI_REPLY_PCM_BLOCK_SAMPLES 512u
/** @brief AI 回复 PCM 预缓冲块数量，约 2048ms 音频。 */
#define APP_AI_REPLY_PCM_BLOCK_COUNT   64u
/** @brief AI 回复播放启动前的预缓冲块数，降低公网 HTTP 分片间隙导致的卡顿。 */
#define APP_AI_REPLY_PLAY_START_BLOCKS 24u
/** @brief AI 回复播放启动前最多等待预缓冲的时间。 */
#define APP_AI_REPLY_PLAY_START_WAIT_MS 1500u
/** @brief AI 回复下载任务栈大小。 */
#define APP_AI_REPLY_FETCH_TASK_STACK  4096u
/** @brief AI 录音任务单次读取超时时间，单位 ms。 */
#define APP_AI_RECORD_READ_TIMEOUT_MS 30u
/** @brief ROP1 固定头：编码参数和精确裁剪信息。 */
#define APP_AI_ROP1_HEADER_LEN         28u
/** @brief 单个 Opus packet 的标准最大字节数。 */
#define APP_AI_ROP1_MAX_PACKET_BYTES   1275u

typedef enum {
    APP_AI_REPLY_MSG_PCM = 1,
    APP_AI_REPLY_MSG_DONE,
} app_ai_voice_reply_msg_type_t;

typedef struct {
    app_ai_voice_reply_msg_type_t type;
    int ret;
    uint16_t block_index;
    uint16_t samples;
} app_ai_voice_reply_msg_t;

typedef struct {
    const char *session;
    uint32_t total;
    osal_queue_t free_queue;
    osal_queue_t filled_queue;
    int16_t *pcm_blocks;
} app_ai_voice_reply_playback_ctx_t;

/**
 * @brief AI 回复 WAV 流式解析状态。
 *
 * HTTP 回复按分片下载，不能假设每次拿到完整 WAV chunk，因此用状态机跨分片
 * 保存 RIFF、chunk header、fmt 和 data 的解析进度。
 */
typedef enum {
    APP_AI_WAV_STREAM_FIND_RIFF = 0,  /**< 扫描字节流，查找 RIFF 魔数。 */
    APP_AI_WAV_STREAM_RIFF_HEADER,    /**< 读取 RIFF 头剩余字段并校验 WAVE 标识。 */
    APP_AI_WAV_STREAM_CHUNK_HEADER,   /**< 读取 8 字节 chunk 头，判断后续 payload 类型。 */
    APP_AI_WAV_STREAM_FMT_PAYLOAD,    /**< 读取 fmt chunk 并校验采样率、声道和位宽。 */
    APP_AI_WAV_STREAM_SKIP_PAYLOAD,   /**< 跳过当前业务不关心的 chunk payload。 */
    APP_AI_WAV_STREAM_DATA_PAYLOAD,   /**< 读取 data chunk，边解析 PCM 边播放。 */
    APP_AI_WAV_STREAM_PAD_BYTE,       /**< 跳过奇数字节 chunk 后的 1 字节对齐填充。 */
} app_ai_voice_wav_stream_state_t;

/**
 * @brief AI 回复 WAV 流式解析上下文。
 *
 * 该结构体只保存解析状态和小型临时字段，不缓存完整回复音频。
 */
typedef struct {
    app_ai_voice_wav_stream_state_t state;           /**< 当前解析状态。 */
    app_ai_voice_wav_stream_state_t state_after_pad; /**< 跳过 pad 字节后恢复到的状态。 */
    uint8_t riff_buf[12];                            /**< RIFF 头缓存：RIFF + size + WAVE。 */
    uint32_t riff_len;                               /**< riff_buf 中已缓存的字节数。 */
    uint32_t riff_match;                             /**< 查找 RIFF 魔数时已经匹配的字节数。 */
    uint8_t chunk_header[8];                         /**< 当前 chunk 头缓存：id + size。 */
    uint32_t chunk_header_len;                       /**< chunk_header 中已缓存的字节数。 */
    uint32_t chunk_size;                             /**< 当前 chunk payload 总长度。 */
    uint32_t chunk_read;                             /**< 当前 chunk payload 已处理长度。 */
    uint8_t fmt_buf[APP_AI_WAV_FMT_MIN_BYTES];       /**< fmt chunk 关键字段缓存。 */
    uint32_t fmt_len;                                /**< fmt_buf 中已缓存的字节数。 */
    uint8_t fmt_valid;                               /**< fmt chunk 是否已通过格式校验。 */
    uint8_t data_started;                            /**< 是否已经遇到并开始处理 data chunk。 */
    uint8_t playback_started;                        /**< 本次回复播放会话是否已启动。 */
    uint8_t pending_pcm_byte;                        /**< 跨分片残留的单个 PCM 低字节。 */
    uint8_t pending_pcm_len;                         /**< pending_pcm_byte 是否有效，0 或 1。 */
    app_ai_voice_reply_playback_ctx_t *playback_ctx; /**< PCM 输出队列上下文。 */
    int16_t emit_buf[APP_AI_REPLY_PCM_BLOCK_SAMPLES]; /**< 待发送到播放队列的 PCM 块。 */
    uint32_t emit_samples;                           /**< emit_buf 中已缓存的样本数。 */
} app_ai_voice_wav_stream_t;

/**
 * @brief 初始化 AI 回复 WAV 流解析上下文。
 *
 * @param[out] stream 待初始化的解析上下文。
 */
static void app_ai_voice_wav_stream_init(app_ai_voice_wav_stream_t *stream,
                                         app_ai_voice_reply_playback_ctx_t *playback_ctx)
{
    memset(stream, 0, sizeof(*stream));
    stream->state = APP_AI_WAV_STREAM_FIND_RIFF;
    stream->state_after_pad = APP_AI_WAV_STREAM_CHUNK_HEADER;
    stream->playback_ctx = playback_ctx;
}

/**
 * @brief 返回两个 uint32_t 中较小的值。
 */
static uint32_t app_ai_voice_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static int app_ai_voice_sha256_hex(const uint8_t *data,
                                   uint32_t len,
                                   char out[APP_AI_WS_SHA256_TEXT_BYTES])
{
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int ret = mbedtls_sha256_starts(&ctx, 0);
    if (ret == 0) {
        ret = mbedtls_sha256_update(&ctx, data, len);
    }
    if (ret == 0) {
        ret = mbedtls_sha256_finish(&ctx, digest);
    }
    mbedtls_sha256_free(&ctx);
    if (ret != 0) {
        return -1;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(digest); i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
    return 0;
}

static int app_ai_voice_prepare_request_identity(const uint8_t *data,
                                                 uint32_t len,
                                                 char request_id[APP_AI_WS_REQUEST_ID_BYTES],
                                                 char sha256[APP_AI_WS_SHA256_TEXT_BYTES])
{
    if (data == NULL || len == 0u || request_id == NULL || sha256 == NULL ||
        app_ai_voice_sha256_hex(data, len, sha256) != 0) {
        return -1;
    }
    s_ai_request_sequence++;
    int written = snprintf(request_id,
                           APP_AI_WS_REQUEST_ID_BYTES,
                           "%s-voice-%08x-%08x-%.8s",
                           APP_DEVICE_ID,
                           (unsigned int)osal_get_tick_ms(),
                           (unsigned int)s_ai_request_sequence,
                           sha256);
    return written > 0 && written < (int)APP_AI_WS_REQUEST_ID_BYTES ? 0 : -2;
}

/** @brief 解码已完整校验的 ROP1；播放阶段不再访问网络。 */
static int app_ai_voice_play_rop1(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len < APP_AI_ROP1_HEADER_LEN ||
        memcmp(data, "ROP1", 4u) != 0 ||
        data[4] != 1u || data[5] != APP_BUSINESS_AUDIO_CHANNELS ||
        app_ai_voice_wav_read_u16(&data[6]) != APP_AI_ROP1_HEADER_LEN ||
        app_ai_voice_wav_read_u32(&data[8]) != APP_BUSINESS_AUDIO_SAMPLE_RATE ||
        app_ai_voice_wav_read_u16(&data[12]) != APP_BUSINESS_FRAME_SAMPLES ||
        app_ai_voice_wav_read_u16(&data[14]) != APP_AI_AOP1_FRAME_DURATION_MS) {
        return -1;
    }

    uint32_t frame_count = app_ai_voice_wav_read_u32(&data[16]);
    uint32_t pcm_samples = app_ai_voice_wav_read_u32(&data[20]);
    uint32_t pre_skip = app_ai_voice_wav_read_u16(&data[24]);
    uint32_t end_trim = app_ai_voice_wav_read_u16(&data[26]);
    uint64_t decoded_samples = (uint64_t)frame_count * APP_BUSINESS_FRAME_SAMPLES;
    uint32_t effective_pre_skip = pre_skip;
    if (frame_count == 0u || pcm_samples == 0u ||
        pcm_samples > APP_BUSINESS_AI_REPLY_MAX_SAMPLES || pcm_samples > decoded_samples) {
        APP_LOGW(TAG,
                 "AI ROP1 头非法, frames=%u pcm=%u pre_skip=%u end_trim=%u",
                 (unsigned int)frame_count,
                 (unsigned int)pcm_samples,
                 (unsigned int)pre_skip,
                 (unsigned int)end_trim);
        return -2;
    }
    if (decoded_samples != (uint64_t)pre_skip + pcm_samples + end_trim) {
        if (decoded_samples == (uint64_t)pcm_samples + end_trim) {
            /* 兼容首版后端把 pre_skip 重复计入 end_trim 的容器，避免整段回复不可播。 */
            effective_pre_skip = 0u;
            APP_LOGW(TAG,
                     "AI ROP1 使用首版 trim 兼容, frames=%u pcm=%u pre_skip=%u end_trim=%u",
                     (unsigned int)frame_count,
                     (unsigned int)pcm_samples,
                     (unsigned int)pre_skip,
                     (unsigned int)end_trim);
        } else {
            return -2;
        }
    }

    uint32_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    cfg.self_delimited = false;
    esp_audio_dec_handle_t decoder = NULL;
    esp_audio_err_t codec_ret = esp_opus_dec_open(&cfg, sizeof(cfg), &decoder);
    if (codec_ret != ESP_AUDIO_ERR_OK || decoder == NULL) {
        APP_LOGE(TAG, "AI ROP1 Opus 解码器创建失败, ret=%d", (int)codec_ret);
        return -3;
    }
    if (esp_opus_dec_reset(decoder) != ESP_AUDIO_ERR_OK) {
        (void)esp_opus_dec_close(decoder);
        return -4;
    }

    int ret = service_audio_start_playback();
    if (ret != 0) {
        (void)esp_opus_dec_close(decoder);
        return ret;
    }

    uint32_t offset = APP_AI_ROP1_HEADER_LEN;
    uint32_t frames = 0u;
    uint32_t skip_left = effective_pre_skip;
    uint32_t emitted = 0u;
    uint64_t decode_total_us = 0u;
    uint32_t decode_max_us = 0u;
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES] __attribute__((aligned(16)));
    while (frames < frame_count) {
        if (app_ai_voice_is_playback_interrupted()) {
            ret = app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
            break;
        }
        if (offset + 2u > len) {
            ret = -5;
            break;
        }
        uint16_t packet_len = app_ai_voice_wav_read_u16(&data[offset]);
        offset += 2u;
        if (packet_len == 0u || packet_len > APP_AI_ROP1_MAX_PACKET_BYTES ||
            offset + packet_len > len) {
            ret = -6;
            break;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = (uint8_t *)&data[offset],
            .len = packet_len,
            .consumed = 0u,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t decoded = {
            .buffer = (uint8_t *)pcm,
            .len = sizeof(pcm),
            .needed_size = 0u,
            .decoded_size = 0u,
        };
        esp_audio_dec_info_t info = {0};
        int64_t decode_start_us = esp_timer_get_time();
        codec_ret = esp_opus_dec_decode(decoder, &raw, &decoded, &info);
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start_us);
        decode_total_us += decode_us;
        if (decode_us > decode_max_us) {
            decode_max_us = decode_us;
        }
        if (codec_ret != ESP_AUDIO_ERR_OK || raw.consumed != packet_len ||
            decoded.decoded_size != sizeof(pcm)) {
            ret = -7;
            break;
        }
        offset += packet_len;
        frames++;

        uint32_t first = skip_left > APP_BUSINESS_FRAME_SAMPLES ?
                         APP_BUSINESS_FRAME_SAMPLES : skip_left;
        skip_left -= first;
        uint32_t available = APP_BUSINESS_FRAME_SAMPLES - first;
        uint32_t wanted = emitted < pcm_samples ? pcm_samples - emitted : 0u;
        if (available > wanted) {
            available = wanted;
        }
        uint32_t played = 0u;
        while (played < available) {
            if (app_ai_voice_is_playback_interrupted()) {
                ret = app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
                break;
            }
            uint32_t chunk = app_ai_voice_min_u32(available - played, APP_AI_PLAY_CHUNK_SAMPLES);
            int play_ret = service_audio_play(&pcm[first + played], chunk, 100u);
            if (play_ret <= 0) {
                ret = play_ret != 0 ? play_ret : -8;
                break;
            }
            played += (uint32_t)play_ret;
            emitted += (uint32_t)play_ret;
        }
        if (ret != 0) {
            break;
        }
    }

    (void)service_audio_stop_playback();
    (void)esp_opus_dec_close(decoder);
    if (ret == 0 && (frames != frame_count || emitted != pcm_samples || offset != len)) {
        ret = -9;
    }
    APP_LOGI(TAG,
             "AI ROP1 播放结束, ret=%d frames=%u pcm=%u decode_avg_us=%u decode_max_us=%u "
             "stack_hwm=%u internal_used=%u psram_used=%u",
             ret,
             (unsigned int)frames,
             (unsigned int)emitted,
             frames > 0u ? (unsigned int)(decode_total_us / frames) : 0u,
             (unsigned int)decode_max_us,
             (unsigned int)uxTaskGetStackHighWaterMark(NULL),
             (unsigned int)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ret;
}

/**
 * @brief 校验 WAV fmt chunk 是否符合本业务支持的 PCM 格式。
 *
 * 当前只支持 16kHz、单声道、16bit、PCM 编码。
 *
 * @param[in,out] stream WAV 流解析上下文。
 * @return 成功返回 0；格式不支持或参数错误返回负值。
 */
static int app_ai_voice_wav_stream_validate_fmt(app_ai_voice_wav_stream_t *stream)
{
    if (stream == NULL || stream->fmt_len < APP_AI_WAV_FMT_MIN_BYTES) {
        return -1;
    }

    uint16_t audio_format = app_ai_voice_wav_read_u16(&stream->fmt_buf[0]);
    uint16_t channels = app_ai_voice_wav_read_u16(&stream->fmt_buf[2]);
    uint32_t sample_rate = app_ai_voice_wav_read_u32(&stream->fmt_buf[4]);
    uint16_t bits = app_ai_voice_wav_read_u16(&stream->fmt_buf[14]);

    if (audio_format != 1u ||
        channels != APP_BUSINESS_AUDIO_CHANNELS ||
        sample_rate != APP_BUSINESS_AUDIO_SAMPLE_RATE ||
        bits != APP_BUSINESS_AUDIO_BITS) {
        return -2;
    }

    stream->fmt_valid = 1u;
    return 0;
}

/**
 * @brief 结束当前 WAV chunk，并根据长度奇偶决定是否跳过 pad 字节。
 *
 * WAV chunk payload 为奇数字节时会附带 1 字节对齐填充，解析器需要显式跳过。
 *
 * @param[in,out] stream WAV 流解析上下文。
 */
static void app_ai_voice_wav_stream_finish_chunk(app_ai_voice_wav_stream_t *stream)
{
    stream->chunk_header_len = 0u;
    stream->chunk_read = 0u;
    if ((stream->chunk_size & 1u) != 0u) {
        stream->state = APP_AI_WAV_STREAM_PAD_BYTE;
        stream->state_after_pad = APP_AI_WAV_STREAM_CHUNK_HEADER;
    } else {
        stream->state = APP_AI_WAV_STREAM_CHUNK_HEADER;
    }
}

static int app_ai_voice_reply_send_pcm_block(app_ai_voice_wav_stream_t *stream)
{
    if (stream == NULL || stream->playback_ctx == NULL || stream->emit_samples == 0u) {
        return -1;
    }

    app_ai_voice_reply_playback_ctx_t *ctx = stream->playback_ctx;
    uint16_t block_index = 0u;
    while (osal_queue_recv(ctx->free_queue, &block_index, 100u) != 0) {
        if (app_ai_voice_is_playback_interrupted()) {
            return app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
        }
    }
    if (block_index >= APP_AI_REPLY_PCM_BLOCK_COUNT) {
        return -3;
    }

    int16_t *block = &ctx->pcm_blocks[(uint32_t)block_index * APP_AI_REPLY_PCM_BLOCK_SAMPLES];
    memcpy(block, stream->emit_buf, stream->emit_samples * sizeof(int16_t));

    app_ai_voice_reply_msg_t msg = {
        .type = APP_AI_REPLY_MSG_PCM,
        .ret = 0,
        .block_index = block_index,
        .samples = (uint16_t)stream->emit_samples,
    };
    while (osal_queue_send(ctx->filled_queue, &msg, 100u) != 0) {
        if (app_ai_voice_is_playback_interrupted()) {
            (void)osal_queue_send(ctx->free_queue, &block_index, OSAL_WAIT_NONE);
            return app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
        }
    }

    stream->emit_samples = 0u;
    return 0;
}

static int app_ai_voice_reply_emit_sample(app_ai_voice_wav_stream_t *stream, int16_t sample)
{
    if (stream == NULL) {
        return -1;
    }

    stream->emit_buf[stream->emit_samples++] = sample;
    return stream->emit_samples >= APP_AI_REPLY_PCM_BLOCK_SAMPLES ?
           app_ai_voice_reply_send_pcm_block(stream) :
           0;
}

static int app_ai_voice_reply_flush_pcm(app_ai_voice_wav_stream_t *stream)
{
    if (stream == NULL || stream->emit_samples == 0u) {
        return 0;
    }

    return app_ai_voice_reply_send_pcm_block(stream);
}

/**
 * @brief 将 data chunk 中的 PCM 字节流转换为 int16_t 并送入播放队列。
 *
 * HTTP 分片可能切在 PCM 样本的两个字节之间，函数会通过 pending_pcm_byte
 * 保存半个样本，等下一片到来后再补齐。
 *
 * @param[in,out] stream WAV 流解析上下文。
 * @param[in] data PCM 字节流。
 * @param[in] len PCM 字节流长度。
 * @return 成功返回 0；入队失败返回负值。
 */
static int app_ai_voice_queue_pcm_bytes(app_ai_voice_wav_stream_t *stream,
                                        const uint8_t *data,
                                        uint32_t len)
{
    if (stream == NULL || (data == NULL && len > 0u)) {
        return -1;
    }

    uint32_t pos = 0u;

    if (stream->pending_pcm_len != 0u && len > 0u) {
        int16_t sample = (int16_t)((uint16_t)stream->pending_pcm_byte | ((uint16_t)data[0] << 8));
        int ret = app_ai_voice_reply_emit_sample(stream, sample);
        if (ret != 0) {
            return ret;
        }
        stream->pending_pcm_len = 0u;
        pos = 1u;
    }

    while ((pos + 1u) < len) {
        int16_t sample = (int16_t)((uint16_t)data[pos] | ((uint16_t)data[pos + 1u] << 8));
        int ret = app_ai_voice_reply_emit_sample(stream, sample);
        if (ret != 0) {
            return ret;
        }
        pos += 2u;
    }

    if (pos < len) {
        stream->pending_pcm_byte = data[pos];
        stream->pending_pcm_len = 1u;
    }

    return 0;
}

/**
 * @brief 向 AI 回复 WAV 流解析器喂入一个 HTTP 分片。
 *
 * 本函数可处理 RIFF/chunk/data 被任意拆分的情况；遇到 data chunk 后会
 * 边解析 PCM 边送入播放队列。
 *
 * @param[in,out] stream WAV 流解析上下文。
 * @param[in] data 当前 HTTP 分片数据。
 * @param[in] len 当前 HTTP 分片长度。
 * @return 成功返回 0；WAV 格式错误或播放失败返回负值。
 */
static int app_ai_voice_wav_stream_feed(app_ai_voice_wav_stream_t *stream,
                                        const uint8_t *data,
                                        uint32_t len)
{
    static const uint8_t riff_magic[] = {'R', 'I', 'F', 'F'};

    if (stream == NULL || (data == NULL && len > 0u)) {
        return -1;
    }

    uint32_t pos = 0u;
    while (pos < len) {
        switch (stream->state) {
        case APP_AI_WAV_STREAM_FIND_RIFF:
            while (pos < len && stream->state == APP_AI_WAV_STREAM_FIND_RIFF) {
                uint8_t b = data[pos++];
                if (b == riff_magic[stream->riff_match]) {
                    stream->riff_match++;
                    if (stream->riff_match == sizeof(riff_magic)) {
                        memcpy(stream->riff_buf, riff_magic, sizeof(riff_magic));
                        stream->riff_len = sizeof(riff_magic);
                        stream->riff_match = 0u;
                        stream->state = APP_AI_WAV_STREAM_RIFF_HEADER;
                    }
                } else {
                    stream->riff_match = b == riff_magic[0] ? 1u : 0u;
                }
            }
            break;

        case APP_AI_WAV_STREAM_RIFF_HEADER: {
            uint32_t need = (uint32_t)sizeof(stream->riff_buf) - stream->riff_len;
            uint32_t take = app_ai_voice_min_u32(need, len - pos);
            memcpy(&stream->riff_buf[stream->riff_len], &data[pos], take);
            stream->riff_len += take;
            pos += take;
            if (stream->riff_len == sizeof(stream->riff_buf)) {
                if (memcmp(&stream->riff_buf[8], "WAVE", 4u) != 0) {
                    return -2;
                }
                stream->state = APP_AI_WAV_STREAM_CHUNK_HEADER;
            }
            break;
        }

        case APP_AI_WAV_STREAM_CHUNK_HEADER: {
            uint32_t need = (uint32_t)sizeof(stream->chunk_header) - stream->chunk_header_len;
            uint32_t take = app_ai_voice_min_u32(need, len - pos);
            memcpy(&stream->chunk_header[stream->chunk_header_len], &data[pos], take);
            stream->chunk_header_len += take;
            pos += take;
            if (stream->chunk_header_len == sizeof(stream->chunk_header)) {
                stream->chunk_size = app_ai_voice_wav_read_u32(&stream->chunk_header[4]);
                stream->chunk_read = 0u;
                if (memcmp(stream->chunk_header, "fmt ", 4u) == 0) {
                    stream->fmt_len = 0u;
                    stream->state = APP_AI_WAV_STREAM_FMT_PAYLOAD;
                } else if (memcmp(stream->chunk_header, "data", 4u) == 0) {
                    if (stream->fmt_valid == 0u || (stream->chunk_size & 1u) != 0u) {
                        return -3;
                    }
                    stream->data_started = 1u;
                    stream->state = APP_AI_WAV_STREAM_DATA_PAYLOAD;
                } else {
                    stream->state = APP_AI_WAV_STREAM_SKIP_PAYLOAD;
                }
                if (stream->chunk_size == 0u) {
                    if (stream->state == APP_AI_WAV_STREAM_FMT_PAYLOAD ||
                        stream->state == APP_AI_WAV_STREAM_DATA_PAYLOAD) {
                        return -4;
                    }
                    app_ai_voice_wav_stream_finish_chunk(stream);
                }
            }
            break;
        }

        case APP_AI_WAV_STREAM_FMT_PAYLOAD: {
            uint32_t remain = stream->chunk_size - stream->chunk_read;
            uint32_t take = app_ai_voice_min_u32(remain, len - pos);
            uint32_t copy_room = APP_AI_WAV_FMT_MIN_BYTES - stream->fmt_len;
            uint32_t copy_len = app_ai_voice_min_u32(take, copy_room);
            if (copy_len > 0u) {
                memcpy(&stream->fmt_buf[stream->fmt_len], &data[pos], copy_len);
                stream->fmt_len += copy_len;
            }
            stream->chunk_read += take;
            pos += take;
            if (stream->chunk_read == stream->chunk_size) {
                int ret = app_ai_voice_wav_stream_validate_fmt(stream);
                if (ret != 0) {
                    return ret;
                }
                app_ai_voice_wav_stream_finish_chunk(stream);
            }
            break;
        }

        case APP_AI_WAV_STREAM_SKIP_PAYLOAD: {
            uint32_t remain = stream->chunk_size - stream->chunk_read;
            uint32_t take = app_ai_voice_min_u32(remain, len - pos);
            stream->chunk_read += take;
            pos += take;
            if (stream->chunk_read == stream->chunk_size) {
                app_ai_voice_wav_stream_finish_chunk(stream);
            }
            break;
        }

        case APP_AI_WAV_STREAM_DATA_PAYLOAD: {
            uint32_t remain = stream->chunk_size - stream->chunk_read;
            uint32_t take = app_ai_voice_min_u32(remain, len - pos);
            int ret = app_ai_voice_queue_pcm_bytes(stream, &data[pos], take);
            if (ret != 0) {
                return ret;
            }
            stream->chunk_read += take;
            pos += take;
            if (stream->chunk_read == stream->chunk_size) {
                app_ai_voice_wav_stream_finish_chunk(stream);
            }
            break;
        }

        case APP_AI_WAV_STREAM_PAD_BYTE:
            pos++;
            stream->state = stream->state_after_pad;
            break;

        default:
            return -5;
        }
    }

    return 0;
}

/**
 * @brief 校验 AI 回复 WAV 流是否完整结束。
 *
 * @param[in] stream WAV 流解析上下文。
 * @return 完整结束返回 0；fmt/data 缺失或 chunk 未完整返回负值。
 */
static int app_ai_voice_wav_stream_finish(app_ai_voice_wav_stream_t *stream)
{
    if (stream == NULL) {
        return -1;
    }
    if (stream->fmt_valid == 0u || stream->data_started == 0u) {
        return -2;
    }
    if (stream->state == APP_AI_WAV_STREAM_FIND_RIFF ||
        stream->state == APP_AI_WAV_STREAM_RIFF_HEADER ||
        stream->state == APP_AI_WAV_STREAM_PAD_BYTE ||
        stream->chunk_header_len != 0u) {
        return -3;
    }
    if ((stream->state == APP_AI_WAV_STREAM_FMT_PAYLOAD ||
         stream->state == APP_AI_WAV_STREAM_SKIP_PAYLOAD ||
         stream->state == APP_AI_WAV_STREAM_DATA_PAYLOAD) &&
        stream->chunk_read != stream->chunk_size) {
        return -4;
    }
    if (stream->pending_pcm_len != 0u) {
        return -5;
    }
    return app_ai_voice_reply_flush_pcm(stream);
}

/**
 * @brief 构造 FastAPI 路由 URL。
 *
 * APP_BUSINESS_HTTP_BASE_URL 只保存服务根地址，例如 http://x.x.x.x:8000。
 * path 保存 FastAPI 路由，例如 APP_BUSINESS_HTTP_ROUTE_AI_UPLOAD；
 * query 保存该路由的查询参数。
 */
static int app_ai_voice_build_url(char *out, size_t out_size, const char *path, const char *query)
{
    if (out == NULL || out_size == 0u || path == NULL || query == NULL) {
        return -1;
    }

    const char *base = APP_BUSINESS_HTTP_BASE_URL;
    size_t base_len = strlen(base);
    const char *path_start = path;
    while (*path_start == '/' && base_len > 0u && base[base_len - 1u] == '/') {
        path_start++;
    }

    const char *sep = query[0] != '\0' ? "?" : "";
    int written = snprintf(out, out_size, "%s%s%s%s", base, path_start, sep, query);
    return (written > 0 && (size_t)written < out_size) ? 0 : -2;
}

/** @brief 从简单 JSON 中读取字符串字段，供 session 解析使用。 */
static int app_ai_voice_json_get_string(const uint8_t *json,
                                        uint32_t len,
                                        const char *key,
                                        char *out,
                                        size_t out_size)
{
    if (json == NULL || key == NULL || out == NULL || out_size == 0u) {
        return -1;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *start = strstr((const char *)json, pattern);
    if (start == NULL || start >= (const char *)json + len) {
        return -2;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        return -3;
    }
    start++;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start != '"') {
        return -4;
    }
    start++;
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return -5;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len >= out_size) {
        copy_len = out_size - 1u;
    }
    memcpy(out, start, copy_len);
    out[copy_len] = '\0';
    return copy_len > 0u ? 0 : -6;
}

/** @brief 从简单 JSON 中读取无符号整数字段。 */
static int app_ai_voice_json_get_u32(const uint8_t *json, uint32_t len, const char *key, uint32_t *value)
{
    if (json == NULL || key == NULL || value == NULL) {
        return -1;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *start = strstr((const char *)json, pattern);
    if (start == NULL || start >= (const char *)json + len) {
        return -2;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        return -3;
    }
    start++;
    while (*start == ' ' || *start == '\t' || *start == '"') {
        start++;
    }

    uint32_t parsed = 0u;
    int digits = 0;
    while (*start >= '0' && *start <= '9') {
        parsed = (parsed * 10u) + (uint32_t)(*start - '0');
        start++;
        digits++;
    }
    if (digits == 0) {
        return -4;
    }

    *value = parsed;
    return 0;
}

/** @brief 判断简单 JSON 中布尔字段是否为 true。 */
static int app_ai_voice_json_is_true(const uint8_t *json, uint32_t len, const char *key)
{
    if (json == NULL || key == NULL) {
        return 0;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *start = strstr((const char *)json, pattern);
    if (start == NULL || start >= (const char *)json + len) {
        return 0;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        return 0;
    }
    start++;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    return strncmp(start, "true", 4u) == 0 ? 1 : 0;
}

static void app_ai_voice_clear_reply_state(void)
{
    s_reply_session[0] = '\0';
    s_reply_wav_size = 0u;
    s_reply_audio_ready = 0;
    s_reply_stop_requested = 0;
    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
}

static void app_ai_voice_store_reply_state(const char *session, uint32_t total)
{
    if (session == NULL || total == 0u) {
        app_ai_voice_clear_reply_state();
        return;
    }

    strncpy(s_reply_session, session, sizeof(s_reply_session) - 1u);
    s_reply_session[sizeof(s_reply_session) - 1u] = '\0';
    s_reply_wav_size = total;
    s_reply_audio_ready = 1;
}

static void app_ai_voice_parse_result_info(const char *session,
                                           const uint8_t *json,
                                           uint32_t len,
                                           app_ai_voice_result_info_t *info)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    if (session != NULL) {
        strncpy(info->session, session, sizeof(info->session) - 1u);
    }
    if (json == NULL || len == 0u) {
        return;
    }

    (void)app_ai_voice_json_get_string(json, len, "status", info->status, sizeof(info->status));
    (void)app_ai_voice_json_get_string(json, len, "tts_status", info->tts_status, sizeof(info->tts_status));
    (void)app_ai_voice_json_get_string(json, len, "tts_error", info->tts_error, sizeof(info->tts_error));
    (void)app_ai_voice_json_get_string(json, len, "answer_text", info->answer_text, sizeof(info->answer_text));
    (void)app_ai_voice_json_get_string(json, len, "session", info->session, sizeof(info->session));

    info->text_ready = (info->answer_text[0] != '\0' ||
                        strcmp(info->status, "text_ready") == 0 ||
                        strcmp(info->status, "audio_ready") == 0 ||
                        strcmp(info->status, "audio_failed") == 0 ||
                        strcmp(info->status, "no_speech") == 0) ? 1u : 0u;
    info->audio_ready = (strcmp(info->status, "audio_ready") == 0 ||
                         app_ai_voice_json_is_true(json, len, "audio_ready") ||
                         app_ai_voice_json_is_true(json, len, "reply_wav_ready") ||
                         app_ai_voice_json_is_true(json, len, "ready")) ? 1u : 0u;
    info->audio_failed = (strcmp(info->status, "audio_failed") == 0 ||
                          strcmp(info->tts_status, "failed") == 0) ? 1u : 0u;
    info->no_speech = strcmp(info->status, "no_speech") == 0 ? 1u : 0u;
    info->canceled = (strcmp(info->status, "cancelled") == 0 ||
                      strcmp(info->status, "canceled") == 0) ? 1u : 0u;
    info->failed = strcmp(info->status, "failed") == 0 ? 1u : 0u;

    if (app_ai_voice_json_get_u32(json, len, "reply_wav_size", &info->total) != 0) {
        if (app_ai_voice_json_get_u32(json, len, "total", &info->total) != 0) {
            info->total = 0u;
        }
    }
}

/** @brief POST 一个 JSON 请求并把响应放入 s_ai_resp_buf。 */
static int app_ai_voice_post_json_body(const char *url,
                                       const uint8_t *json,
                                       uint32_t json_len,
                                       uint32_t *resp_len)
{
    return service_network_http_post(url,
                                     "application/json",
                                     json,
                                     json_len,
                                     s_ai_resp_buf,
                                     sizeof(s_ai_resp_buf) - 1u,
                                     resp_len,
                                     APP_AI_HTTP_CHUNK_TIMEOUT_MS);
}

/** @brief POST 空 JSON 请求并把响应放入 s_ai_resp_buf。 */
static int app_ai_voice_post_json(const char *url, uint32_t *resp_len)
{
    static const uint8_t empty_json[] = "{}";

    return app_ai_voice_post_json_body(url, empty_json, sizeof(empty_json) - 1u, resp_len);
}

#if APP_AI_BACKEND_CANCEL_ON_DEVICE
static int app_ai_voice_send_backend_cancel(const char *session)
{
    char query[128];
    char url[256];
    uint8_t resp[128];
    uint32_t resp_len = 0u;
    static const uint8_t empty_json[] = "{}";

    if (session == NULL || session[0] == '\0') {
        return -1;
    }

    snprintf(query, sizeof(query), "session=%s&device=%s", session, APP_DEVICE_ID);
    if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_CANCEL, query) != 0) {
        return -2;
    }

    int ret = service_network_http_post(url,
                                        "application/json",
                                        empty_json,
                                        sizeof(empty_json) - 1u,
                                        resp,
                                        sizeof(resp) - 1u,
                                        &resp_len,
                                        APP_AI_CANCEL_HTTP_TIMEOUT_MS);
    if (ret == 0) {
        APP_LOGI(CANCEL_TAG, "backend cancel request sent, session=%s", session);
    } else {
        APP_LOGW(CANCEL_TAG, "backend cancel failed but local canceled, session=%s, ret=%d", session, ret);
    }
    return ret;
}

static void app_ai_voice_backend_cancel_task(void *arg)
{
    (void)arg;

    char session[64];
    strncpy(session, s_ai_cancel_task_session, sizeof(session) - 1u);
    session[sizeof(session) - 1u] = '\0';

    if (session[0] != '\0') {
        APP_LOGI(CANCEL_TAG, "cancel current session, session=%s", session);
        (void)app_ai_voice_send_backend_cancel(session);
    }

    s_ai_cancel_task_busy = 0;
    osal_task_delete_current();
}

static void app_ai_voice_request_backend_cancel_async(const char *session)
{
    if (session == NULL || session[0] == '\0' || s_ai_cancel_task_busy) {
        return;
    }

    strncpy(s_ai_cancel_task_session, session, sizeof(s_ai_cancel_task_session) - 1u);
    s_ai_cancel_task_session[sizeof(s_ai_cancel_task_session) - 1u] = '\0';
    s_ai_cancel_task_busy = 1;
    if (osal_task_create("ai_cancel",
                         app_ai_voice_backend_cancel_task,
                         NULL,
                         3072u,
                         5u,
                         NULL) != 0) {
        s_ai_cancel_task_busy = 0;
        APP_LOGW(CANCEL_TAG, "backend cancel task create failed");
    }
}
#else
static void app_ai_voice_request_backend_cancel_async(const char *session)
{
    (void)session;
    (void)app_ai_ws_cancel_active();
}
#endif

/** @brief 请求服务器创建一次 AI 会话。 */
static int app_ai_voice_start_session(char *session, size_t session_size)
{
    char json[128];
    char url[256];
    char audio_format[32];
    uint32_t resp_len = 0u;

    if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_START, "") != 0) {
        return -1;
    }

    int written = snprintf(json,
                           sizeof(json),
                           "{\"device\":\"%s\",\"language\":\"zh\","
                           "\"audio_format\":\"%s\"}",
                           APP_DEVICE_ID,
                           APP_AI_AUDIO_FORMAT);
    if (written <= 0 || (size_t)written >= sizeof(json)) {
        return -2;
    }

    if (app_ai_voice_is_cancel_requested()) {
        return APP_AI_CANCELED_RET;
    }

    int ret = app_ai_voice_post_json_body(url, (const uint8_t *)json, (uint32_t)written, &resp_len);
    if (ret != 0) {
        return ret;
    }
    s_ai_resp_buf[resp_len < sizeof(s_ai_resp_buf) ? resp_len : (sizeof(s_ai_resp_buf) - 1u)] = '\0';
    if (!app_ai_voice_json_is_true(s_ai_resp_buf, resp_len, "ok")) {
        APP_LOGE(TAG, "ai_opus event=start_rejected reason=server_not_ok");
        return -3;
    }
    ret = app_ai_voice_json_get_string(s_ai_resp_buf, resp_len, "session", session, session_size);
    if (ret != 0) {
        APP_LOGE(TAG, "ai_opus event=start_rejected reason=missing_session ret=%d", ret);
        return -4;
    }
    ret = app_ai_voice_json_get_string(s_ai_resp_buf,
                                       resp_len,
                                       "audio_format",
                                       audio_format,
                                       sizeof(audio_format));
    if (ret != 0) {
        APP_LOGE(TAG, "ai_opus event=start_rejected reason=missing_audio_format ret=%d", ret);
        return -5;
    }
    if (strcmp(audio_format, APP_AI_AUDIO_FORMAT) != 0) {
        APP_LOGE(TAG,
                 "ai_opus event=start_rejected reason=unsupported_audio_format got=%s expected=%s",
                 audio_format,
                 APP_AI_AUDIO_FORMAT);
        return -6;
    }
    if (app_ai_voice_is_cancel_requested()) {
        app_ai_voice_set_current_session(session);
        app_ai_voice_request_backend_cancel_async(session);
        return APP_AI_CANCELED_RET;
    }
    return 0;
}

/** @brief 按文件字节偏移分片上传完整 AOP1 请求音频。 */
static int app_ai_voice_upload_opus_chunks(const char *session, uint32_t total_len)
{
    if (session == NULL || total_len <= APP_AI_AOP1_HEADER_LEN ||
        total_len > APP_BUSINESS_AI_OPUS_BUF_BYTES) {
        return -1;
    }

    uint32_t upload_start_ms = osal_get_tick_ms();
    uint32_t offset = 0u;
    uint32_t index = 0u;
    uint32_t chunk_count = 0u;
    int result = 0;
    while (offset < total_len) {
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during upload index=%u", (unsigned int)index);
            result = APP_AI_CANCELED_RET;
            break;
        }

        uint32_t chunk_len = total_len - offset;
        if (chunk_len > APP_AI_UPLOAD_CHUNK_BYTES) {
            chunk_len = APP_AI_UPLOAD_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        snprintf(query,
                 sizeof(query),
                 "session=%s&device=%s&index=%u&offset=%u&total=%u",
                 session,
                 APP_DEVICE_ID,
                 (unsigned int)index,
                 (unsigned int)offset,
                 (unsigned int)total_len);
        if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_UPLOAD, query) != 0) {
            result = -2;
            break;
        }

        APP_LOGI(TAG,
                 "AI 上传分片, index=%u, offset=%u, len=%u, total=%u",
                 (unsigned int)index,
                 (unsigned int)offset,
                 (unsigned int)chunk_len,
                 (unsigned int)total_len);
        chunk_count++;
        int ret = service_network_http_post(url,
                                            "application/vnd.wkt.opus-packets",
                                            &s_ai_request_audio_buf[offset],
                                            chunk_len,
                                            s_ai_resp_buf,
                                            sizeof(s_ai_resp_buf) - 1u,
                                            &resp_len,
                                            APP_AI_HTTP_CHUNK_TIMEOUT_MS);
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during upload index=%u", (unsigned int)index);
            result = APP_AI_CANCELED_RET;
            break;
        }
        if (ret != 0) {
            APP_LOGW(TAG, "AI 上传分片失败, index=%u, offset=%u, ret=%d",
                     (unsigned int)index,
                     (unsigned int)offset,
                     ret);
            result = ret;
            break;
        }

        offset += chunk_len;
        index++;
    }

    APP_LOGI(TAG,
             "ai_opus event=upload_stop total_bytes=%u chunk_count=%u upload_ms=%u result=%d",
             (unsigned int)total_len,
             (unsigned int)chunk_count,
             (unsigned int)(osal_get_tick_ms() - upload_start_ms),
             result);
    return result;
}

/** @brief 通知服务器上传结束并开始处理。 */
static int app_ai_voice_finish_upload(const char *session)
{
    char query[128];
    char url[256];
    uint32_t resp_len = 0u;

    snprintf(query, sizeof(query), "session=%s&device=%s", session, APP_DEVICE_ID);
    if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_FINISH, query) != 0) {
        return -1;
    }

    if (app_ai_voice_is_cancel_requested()) {
        return APP_AI_CANCELED_RET;
    }

    int ret = service_network_http_post(url,
                                        "application/json",
                                        NULL,
                                        0u,
                                        s_ai_resp_buf,
                                        sizeof(s_ai_resp_buf) - 1u,
                                        &resp_len,
                                        APP_AI_HTTP_CHUNK_TIMEOUT_MS);
    if (app_ai_voice_is_cancel_requested()) {
        return APP_AI_CANCELED_RET;
    }
    APP_LOGI("AI-UI", "finish sent");
    if (ret != 0) {
        APP_LOGW(TAG, "AI finish response timeout/error ignored, continue polling result_info, ret=%d", ret);
    }
    return 0;
}

/** @brief 轮询服务器，先显示文本，音频准备好后点亮播放按钮。 */
static int app_ai_voice_wait_result_info(const char *session, uint32_t *total)
{
    if (session == NULL || total == NULL) {
        return -1;
    }

    int text_shown = 0;
    uint8_t audio_wait_logged = 0u;
    uint32_t start = osal_get_tick_ms();
    while ((osal_get_tick_ms() - start) < APP_AI_PROCESS_TIMEOUT_MS) {
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during result_info polling");
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_active_session(session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", session);
            return APP_AI_CANCELED_RET;
        }

        char query[128];
        char url[256];
        uint32_t resp_len = 0u;

        snprintf(query, sizeof(query), "session=%s&device=%s", session, APP_DEVICE_ID);
        if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_RESULT_INFO, query) != 0) {
            return -2;
        }

        APP_LOGI("AI-UI", "polling result_info");
        int ret = app_ai_voice_post_json(url, &resp_len);
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during result_info polling");
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_active_session(session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", session);
            return APP_AI_CANCELED_RET;
        }
        if (ret == 0) {
            s_ai_resp_buf[resp_len < sizeof(s_ai_resp_buf) ? resp_len : (sizeof(s_ai_resp_buf) - 1u)] = '\0';
            app_ai_voice_result_info_t *info = &s_ai_result_info;
            app_ai_voice_parse_result_info(session, s_ai_resp_buf, resp_len, info);

            if (info->session[0] != '\0' && strcmp(info->session, session) != 0) {
                APP_LOGW(CANCEL_TAG,
                         "old session response ignored, active=%s, got=%s",
                         session,
                         info->session);
                return APP_AI_CANCELED_RET;
            }

            if (info->canceled) {
                APP_LOGI(CANCEL_TAG, "backend reports session cancelled, session=%s", session);
                return APP_AI_CANCELED_RET;
            }

            if (info->failed) {
                APP_LOGW("AI-UI", "backend reports session failed");
                (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                return -6;
            }

            if (info->text_ready && !text_shown) {
                APP_LOGI("AI-UI", "text_ready answer_len=%u", (unsigned int)strlen(info->answer_text));
                if (info->answer_text[0] != '\0') {
                    (void)app_ui_set_ai_answer_text(info->answer_text);
                }
                (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_WAITING);
                text_shown = 1;
                app_ai_voice_set_state(APP_AI_STATE_WAITING_AUDIO);
            }

            if (info->audio_failed) {
                APP_LOGW("AI-UI", "audio_failed error=%s", info->tts_error[0] != '\0' ? info->tts_error : info->tts_status);
                if (info->answer_text[0] != '\0') {
                    (void)app_ui_set_ai_answer_text(info->answer_text);
                } else {
                    (void)app_ui_set_ai_answer_text("语音生成失败");
                }
                (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                return -4;
            }

            if (info->audio_ready) {
                if (info->total == 0u) {
                    (void)app_ai_voice_json_get_u32(s_ai_resp_buf, resp_len, "total", &info->total);
                }
                if (info->total > 0u) {
                    *total = info->total;
                    APP_LOGI("AI-UI", "audio_ready wav_size=%u", (unsigned int)*total);
                    if (!text_shown && info->answer_text[0] == '\0') {
                        (void)app_ui_set_ai_answer_text("语音已生成，点击播放。");
                    }
                    app_ai_voice_store_reply_state(session, *total);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                    return 0;
                }
            }

            if (info->no_speech) {
                APP_LOGI("AI-UI", "no_speech");
                if (info->answer_text[0] != '\0') {
                    (void)app_ui_set_ai_answer_text(info->answer_text);
                } else {
                    (void)app_ui_set_ai_answer_text("我没有听清，请再说一遍。");
                }
                (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
                return APP_AI_TEXT_ONLY_RET;
            }

            if (info->text_ready && audio_wait_logged == 0u) {
                APP_LOGI("AI-UI", "audio_waiting");
                audio_wait_logged = 1u;
            }
        }

        uint32_t slept = 0u;
        while (slept < APP_AI_RESULT_POLL_MS && !app_ai_voice_is_cancel_requested()) {
            osal_delay_ms(100u);
            slept += 100u;
        }
    }

    if (text_shown) {
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
        return -5;
    }
    return -3;
}

static void app_ai_voice_reply_send_done(app_ai_voice_reply_playback_ctx_t *ctx, int ret)
{
    if (ctx == NULL || ctx->filled_queue == NULL) {
        return;
    }

    app_ai_voice_reply_msg_t msg = {
        .type = APP_AI_REPLY_MSG_DONE,
        .ret = ret,
        .block_index = 0u,
        .samples = 0u,
    };
    while (osal_queue_send(ctx->filled_queue, &msg, 100u) != 0) {
    }
}

/** @brief 下载回复 WAV 分片，解析出 PCM 后写入播放队列。 */
static int app_ai_voice_fetch_result_chunks(app_ai_voice_reply_playback_ctx_t *ctx)
{
    if (ctx == NULL || ctx->session == NULL || ctx->total < APP_BUSINESS_WAV_HEADER_LEN ||
        ctx->total > APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES ||
        s_ai_reply_chunk_buf == NULL) {
        return -1;
    }

    app_ai_voice_wav_stream_t stream;
    app_ai_voice_wav_stream_init(&stream, ctx);

    uint32_t offset = 0u;
    while (offset < ctx->total) {
        if (app_ai_voice_is_playback_interrupted()) {
            APP_LOGI(CANCEL_TAG, "reply download interrupted");
            return app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
        }
        if (!app_ai_voice_is_active_session(ctx->session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", ctx->session);
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_audio_fetch_allowed(ctx->session)) {
            APP_LOGI("AI-UI", "reply download stopped by state");
            return APP_AI_PLAY_STOPPED_RET;
        }

        uint32_t chunk_len = ctx->total - offset;
        if (chunk_len > APP_AI_REPLY_CHUNK_BYTES) {
            chunk_len = APP_AI_REPLY_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        static const uint8_t empty_json[] = "{}";
        snprintf(query,
                 sizeof(query),
                 "session=%s&device=%s&offset=%u&len=%u",
                 ctx->session,
                 APP_DEVICE_ID,
                 (unsigned int)offset,
                 (unsigned int)chunk_len);
        if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_RESULT_CHUNK, query) != 0) {
            return -2;
        }

        int ret = service_network_http_post(url,
                                            "application/json",
                                            empty_json,
                                            sizeof(empty_json) - 1u,
                                            s_ai_reply_chunk_buf,
                                            chunk_len,
                                            &resp_len,
                                            APP_AI_HTTP_CHUNK_TIMEOUT_MS);
        if (app_ai_voice_is_playback_interrupted()) {
            APP_LOGI(CANCEL_TAG, "reply download interrupted");
            return app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
        }
        if (!app_ai_voice_is_active_session(ctx->session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", ctx->session);
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_audio_fetch_allowed(ctx->session)) {
            APP_LOGI("AI-UI", "reply chunk ignored after playback state changed");
            return APP_AI_PLAY_STOPPED_RET;
        }
        if (ret != 0 ||
            resp_len == 0u ||
            resp_len > chunk_len ||
            (resp_len < chunk_len && offset + resp_len < ctx->total)) {
            APP_LOGW(TAG,
                      "AI 回复分片下载失败, offset=%u, expect=%u, got=%u, ret=%d",
                      (unsigned int)offset,
                      (unsigned int)chunk_len,
                      (unsigned int)resp_len,
                     ret);
            return ret != 0 ? ret : -3;
        }

        ret = app_ai_voice_wav_stream_feed(&stream, s_ai_reply_chunk_buf, resp_len);
        if (ret != 0) {
            APP_LOGW(TAG,
                     "AI 回复 WAV 流式解析/播放失败, offset=%u, len=%u, ret=%d",
                     (unsigned int)offset,
                     (unsigned int)resp_len,
                     ret);
            return ret;
        }

        offset += resp_len;
    }

    return app_ai_voice_wav_stream_finish(&stream);
}

static void app_ai_voice_reply_fetch_task(void *arg)
{
    app_ai_voice_reply_playback_ctx_t *ctx = (app_ai_voice_reply_playback_ctx_t *)arg;
    int ret = app_ai_voice_fetch_result_chunks(ctx);
    app_ai_voice_reply_send_done(ctx, ret);
    osal_task_delete_current();
}

static int app_ai_voice_play_reply_queue(app_ai_voice_reply_playback_ctx_t *ctx)
{
    if (ctx == NULL || ctx->filled_queue == NULL || ctx->free_queue == NULL || ctx->pcm_blocks == NULL) {
        return -1;
    }

    int playback_started = 0;
    int final_ret = 0;
    uint32_t prebuffer_start = osal_get_tick_ms();
    uint32_t underrun_log_ms = 0u;

    while (1) {
        if (app_ai_voice_is_playback_interrupted() && final_ret == 0) {
            APP_LOGI(CANCEL_TAG, "reply playback interrupted");
            final_ret = app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
        }

        if (!playback_started && final_ret == 0) {
            uint32_t queued = osal_queue_get_count(ctx->filled_queue);
            uint32_t waited = osal_get_tick_ms() - prebuffer_start;
            if (queued < APP_AI_REPLY_PLAY_START_BLOCKS &&
                waited < APP_AI_REPLY_PLAY_START_WAIT_MS) {
                osal_delay_ms(20u);
                continue;
            }
        }

        app_ai_voice_reply_msg_t msg;
        if (osal_queue_recv(ctx->filled_queue, &msg, 100u) != 0) {
            if (playback_started) {
                uint32_t now = osal_get_tick_ms();
                if (now - underrun_log_ms >= 1000u) {
                    APP_LOGW("AI-UI", "reply playback waiting for pcm, queued=%u",
                             (unsigned int)osal_queue_get_count(ctx->filled_queue));
                    underrun_log_ms = now;
                }
            }
            continue;
        }

        if (msg.type == APP_AI_REPLY_MSG_DONE) {
            if (final_ret == 0) {
                final_ret = msg.ret;
            }
            break;
        }

        if (msg.type != APP_AI_REPLY_MSG_PCM ||
            msg.block_index >= APP_AI_REPLY_PCM_BLOCK_COUNT ||
            msg.samples == 0u ||
            msg.samples > APP_AI_REPLY_PCM_BLOCK_SAMPLES) {
            if (final_ret == 0) {
                final_ret = -3;
            }
            continue;
        }

        if (final_ret != 0) {
            (void)osal_queue_send(ctx->free_queue, &msg.block_index, OSAL_WAIT_FOREVER);
            continue;
        }

        if (!playback_started) {
            int ret = service_audio_start_playback();
            if (ret != 0) {
                if (final_ret == 0) {
                    final_ret = ret;
                }
                (void)osal_queue_send(ctx->free_queue, &msg.block_index, OSAL_WAIT_FOREVER);
                continue;
            }
            playback_started = 1;
            APP_LOGI("AI-UI", "playing reply wav");
        }

        int16_t *pcm = &ctx->pcm_blocks[(uint32_t)msg.block_index * APP_AI_REPLY_PCM_BLOCK_SAMPLES];
        uint32_t played = 0u;
        while (played < msg.samples) {
            if (app_ai_voice_is_playback_interrupted()) {
                APP_LOGI(CANCEL_TAG, "reply playback interrupted");
                final_ret = app_ai_voice_is_cancel_requested() ? APP_AI_CANCELED_RET : APP_AI_PLAY_STOPPED_RET;
                break;
            }
            uint32_t remain = (uint32_t)msg.samples - played;
            uint32_t chunk = remain > APP_AI_PLAY_CHUNK_SAMPLES ? APP_AI_PLAY_CHUNK_SAMPLES : remain;
            int ret = service_audio_play(&pcm[played], chunk, 100u);
            if (ret <= 0) {
                if (final_ret == 0) {
                    final_ret = ret != 0 ? ret : -4;
                }
                break;
            }
            played += (uint32_t)ret;
        }

        (void)osal_queue_send(ctx->free_queue, &msg.block_index, OSAL_WAIT_FOREVER);
    }

    if (playback_started) {
        (void)service_audio_stop_playback();
    }
    return final_ret;
}

/** @brief 按 offset/len 拉取回复 WAV 分片，后台预取、前台连续播放。 */
static int app_ai_voice_play_result_chunks(const char *session, uint32_t total)
{
    if (session == NULL || total < APP_BUSINESS_WAV_HEADER_LEN ||
        total > APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES ||
        s_ai_reply_chunk_buf == NULL) {
        return -1;
    }

    if (!app_ai_voice_is_active_session(session)) {
        APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", session);
        return APP_AI_CANCELED_RET;
    }

    APP_LOGI("AI-UI", "downloading reply wav");
    app_ai_voice_set_state(APP_AI_STATE_DOWNLOADING_AUDIO);

    osal_queue_t free_queue = osal_queue_create(APP_AI_REPLY_PCM_BLOCK_COUNT, sizeof(uint16_t));
    osal_queue_t filled_queue = osal_queue_create(APP_AI_REPLY_PCM_BLOCK_COUNT + 1u, sizeof(app_ai_voice_reply_msg_t));
    int16_t *pcm_blocks = (int16_t *)osal_heap_alloc_external(APP_AI_REPLY_PCM_BLOCK_COUNT *
                                                             APP_AI_REPLY_PCM_BLOCK_SAMPLES *
                                                             sizeof(int16_t));
    if (free_queue == NULL || filled_queue == NULL || pcm_blocks == NULL) {
        if (free_queue != NULL) {
            osal_queue_delete(free_queue);
        }
        if (filled_queue != NULL) {
            osal_queue_delete(filled_queue);
        }
        if (pcm_blocks != NULL) {
            osal_heap_free(pcm_blocks);
        }
        return -2;
    }

    for (uint16_t i = 0u; i < APP_AI_REPLY_PCM_BLOCK_COUNT; i++) {
        (void)osal_queue_send(free_queue, &i, OSAL_WAIT_NONE);
    }

    app_ai_voice_reply_playback_ctx_t ctx = {
        .session = session,
        .total = total,
        .free_queue = free_queue,
        .filled_queue = filled_queue,
        .pcm_blocks = pcm_blocks,
    };

    osal_task_t fetch_task = NULL;
    int ret = osal_task_create("ai_reply_fetch",
                               app_ai_voice_reply_fetch_task,
                               &ctx,
                               APP_AI_REPLY_FETCH_TASK_STACK,
                               5u,
                               &fetch_task);
    if (ret == 0) {
        app_ai_voice_set_state(APP_AI_STATE_PLAYING_AUDIO);
        ret = app_ai_voice_play_reply_queue(&ctx);
    }

    osal_queue_delete(free_queue);
    osal_queue_delete(filled_queue);
    osal_heap_free(pcm_blocks);
    return ret;
}

static void app_ai_voice_play_request_task(void *arg)
{
    (void)arg;

    char session[64];
    uint32_t total = 0u;

    if (!s_reply_audio_ready || s_reply_session[0] == '\0' || s_reply_wav_size == 0u) {
        APP_LOGW("AI-UI", "play button disabled, audio not ready");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
        s_reply_play_busy = 0;
        osal_task_delete_current();
        return;
    }

    if (app_business_audio_session_try_begin() != 0) {
        APP_LOGW("AI-UI", "play request ignored, audio session busy");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
        s_reply_play_busy = 0;
        osal_task_delete_current();
        return;
    }

    strncpy(session, s_reply_session, sizeof(session) - 1u);
    session[sizeof(session) - 1u] = '\0';
    total = s_reply_wav_size;
    if (!app_ai_voice_is_active_session(session)) {
        APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", session);
        s_reply_play_busy = 0;
        app_business_audio_session_end();
        osal_task_delete_current();
        return;
    }

    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_PLAYING);
    app_ai_voice_set_state(APP_AI_STATE_PLAYING_AUDIO);
    int ret = app_ai_voice_play_rop1(s_ai_reply_chunk_buf, total);
    if (ret == 0) {
        APP_LOGI("AI-UI", "play done");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
        app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
    } else if (ret == APP_AI_PLAY_STOPPED_RET) {
        APP_LOGI("AI-UI", "play stopped");
        s_reply_stop_requested = 0;
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
        app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
    } else if (ret == APP_AI_CANCELED_RET) {
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_message(UI_TEXT_AI_IDLE);
        app_ai_voice_clear_reply_state();
        app_ai_voice_set_current_session(NULL);
        app_ai_voice_set_state(APP_AI_STATE_CANCELED);
        s_ai_cancel_requested = 0;
    } else {
        APP_LOGW(TAG, "AI 回复播放失败, ret=%d", ret);
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
        app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
    }

    app_business_audio_session_end();
    s_reply_play_busy = 0;
    osal_task_delete_current();
}

typedef struct {
    uint32_t duration_ms;
    uint32_t pcm_samples;
    uint32_t frame_count;
    uint32_t opus_payload_bytes;
    uint32_t container_bytes;
    uint32_t encode_ok;
    uint32_t encode_fail;
    uint64_t encode_total_us;
    uint32_t encode_max_us;
    uint8_t canceled;
    uint8_t buffer_full;
} app_ai_voice_record_result_t;

/** @brief 编码并安全追加一个 AOP1 packet；real_samples 不包含补零样本。 */
static int app_ai_voice_aop1_append_frame(app_ai_voice_record_result_t *result,
                                           const int16_t *pcm,
                                           uint16_t real_samples)
{
    uint8_t packet[APP_AUDIO_OPUS_PACKET_MAX_BYTES] __attribute__((aligned(16)));
    uint16_t packet_len = 0u;
    int64_t encode_start_us = esp_timer_get_time();
    int ret = app_audio_opus_encode_20ms(pcm,
                                          APP_BUSINESS_FRAME_SAMPLES,
                                          packet,
                                          (uint16_t)sizeof(packet),
                                          &packet_len);
    uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_start_us);
    result->encode_total_us += encode_us;
    if (encode_us > result->encode_max_us) {
        result->encode_max_us = encode_us;
    }
    if (ret != 0 || packet_len == 0u || packet_len > APP_AUDIO_OPUS_PACKET_MAX_BYTES) {
        result->encode_fail++;
        APP_LOGE(TAG,
                 "ai_opus event=encode_fail ret=%d packet_len=%u frame_count=%u",
                 ret,
                 (unsigned int)packet_len,
                 (unsigned int)result->frame_count);
        return -1;
    }
    result->encode_ok++;

    uint32_t needed = APP_AI_AOP1_PACKET_LEN_BYTES + packet_len;
    if (result->container_bytes > APP_BUSINESS_AI_OPUS_BUF_BYTES - needed) {
        result->buffer_full = 1u;
        APP_LOGE(TAG,
                 "ai_opus event=buffer_full used=%u needed=%u capacity=%u frame_count=%u",
                 (unsigned int)result->container_bytes,
                 (unsigned int)needed,
                 (unsigned int)APP_BUSINESS_AI_OPUS_BUF_BYTES,
                 (unsigned int)result->frame_count);
        return -2;
    }

    app_ai_voice_write_u16_le(&s_ai_request_audio_buf[result->container_bytes], packet_len);
    memcpy(&s_ai_request_audio_buf[result->container_bytes + APP_AI_AOP1_PACKET_LEN_BYTES],
           packet,
           packet_len);
    result->container_bytes += needed;
    result->opus_payload_bytes += packet_len;
    result->pcm_samples += real_samples;
    result->frame_count++;
    return 0;
}

/** @brief 录制并逐帧编码一次 AI 请求，输出完整 AOP1 文件到 PSRAM。 */
static app_ai_voice_record_result_t app_ai_voice_record_to_opus_buffer(void)
{
    app_ai_voice_record_result_t result = {
        .container_bytes = APP_AI_AOP1_HEADER_LEN,
    };
    int16_t frame[APP_BUSINESS_FRAME_SAMPLES] __attribute__((aligned(16)));
    uint16_t frame_samples = 0u;
    uint32_t record_start_ms = osal_get_tick_ms();

    app_ai_voice_aop1_write_header(s_ai_request_audio_buf, 0u, 0u);
    if (app_audio_opus_encoder_reset() != 0) {
        result.encode_fail = 1u;
        s_ai_recording = 0;
        APP_LOGE(TAG, "ai_opus event=encoder_reset_fail");
        goto done;
    }

    while (s_ai_recording && result.frame_count < APP_AI_AOP1_MAX_FRAMES) {
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during recording");
            result.canceled = 1u;
            s_ai_recording = 0;
            break;
        }

        uint32_t pending_total = result.pcm_samples + frame_samples;
        if (pending_total >= APP_BUSINESS_AI_MAX_SAMPLES) {
            s_ai_recording = 0;
            break;
        }
        uint32_t request = APP_BUSINESS_FRAME_SAMPLES - frame_samples;
        uint32_t sample_room = APP_BUSINESS_AI_MAX_SAMPLES - pending_total;
        if (request > sample_room) {
            request = sample_room;
        }
        int read_samples = service_audio_read(&frame[frame_samples],
                                              request,
                                              APP_AI_RECORD_READ_TIMEOUT_MS);
        if (read_samples <= 0) {
            osal_delay_ms(5u);
            continue;
        }
        if ((uint32_t)read_samples > request) {
            APP_LOGW(TAG,
                     "ai_opus event=read_clamped got=%d requested=%u",
                     read_samples,
                     (unsigned int)request);
            read_samples = (int)request;
        }
        frame_samples = (uint16_t)(frame_samples + (uint16_t)read_samples);
        if (frame_samples < APP_BUSINESS_FRAME_SAMPLES) {
            continue;
        }

        if (app_ai_voice_aop1_append_frame(&result,
                                            frame,
                                            APP_BUSINESS_FRAME_SAMPLES) != 0) {
            frame_samples = 0u;
            s_ai_recording = 0;
            break;
        }
        frame_samples = 0u;
    }

    if (result.frame_count >= APP_AI_AOP1_MAX_FRAMES ||
        result.pcm_samples >= APP_BUSINESS_AI_MAX_SAMPLES) {
        s_ai_recording = 0;
    }

    /* 松手时将最后不足 20 ms 的真实 PCM 以零补齐后编码。 */
    if (!result.canceled && frame_samples > 0u &&
        result.frame_count < APP_AI_AOP1_MAX_FRAMES) {
        uint16_t real_samples = frame_samples;
        memset(&frame[frame_samples],
               0,
               (APP_BUSINESS_FRAME_SAMPLES - frame_samples) * sizeof(int16_t));
        (void)app_ai_voice_aop1_append_frame(&result, frame, real_samples);
    }

done:
    app_ai_voice_aop1_write_header(s_ai_request_audio_buf,
                                    result.frame_count,
                                    result.pcm_samples);
    result.duration_ms = osal_get_tick_ms() - record_start_ms;
    APP_LOGI(TAG,
             "ai_opus event=record_stop duration_ms=%u pcm_samples=%u frame_count=%u "
             "opus_payload_bytes=%u container_bytes=%u encode_ok=%u encode_fail=%u "
             "encode_avg_us=%u encode_max_us=%u stack_hwm=%u psram_free=%u internal_free=%u",
             (unsigned int)result.duration_ms,
             (unsigned int)result.pcm_samples,
             (unsigned int)result.frame_count,
             (unsigned int)result.opus_payload_bytes,
             (unsigned int)result.container_bytes,
             (unsigned int)result.encode_ok,
             (unsigned int)result.encode_fail,
             (result.encode_ok + result.encode_fail) > 0u ?
             (unsigned int)(result.encode_total_us /
                            (result.encode_ok + result.encode_fail)) : 0u,
             (unsigned int)result.encode_max_us,
             (unsigned int)uxTaskGetStackHighWaterMark(NULL),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return result;
}

/* ==========================================================================
 * AI 语音处理任务
 * ========================================================================== */

/**
 * @brief AI 语音处理后台任务入口。
 *
 * ## 执行流程（每次被 notify 唤醒一次→处理一次完整的问答）：
 *
 * 1. 阻塞等待 notify（来自 app_ai_voice_record_start()）
 * 2. 循环读取 service_audio PCM，编码并写入 AOP1 裸 Opus packet
 * 3. 回填 24 字节 AOP1 文件头
 * 5. 通过独立 AI WebSocket 停等上传 AOP1，断线时按服务端 next_offset 续传
 * 6. 等待服务端结果并把 ROP1/Opus 回复完整下载、校验到 PSRAM
 * 7. 用户点击播放后本地解码，不让网络抖动进入播放时序
 * 8. 录音结束后立即释放音频会话，后续网络处理不再阻塞 PTT
 * 9. 回到步骤 1，等待下一次录音完成
 *
 * ## 错误处理
 * - 录音采集结束后立即释放锁，后续任一步骤失败都不会影响 PTT 抢占音频
 * - AI 回复手动播放会重新申请音频会话，和 PTT 继续互斥
 *
 * ## 内存策略
 * - 请求 AOP1 使用 s_ai_request_audio_buf；仅保留单帧 PCM 和 Opus 临时缓冲
 * - 回复 ROP1 完整保存在 PSRAM，长度和 SHA-256 都通过后才开放播放按钮
 *
 * @param arg 未使用。
 */
static void app_ai_voice_task(void *arg)
{
    (void)arg;

    while (1) {
        /* 阻塞等待：AI 按下后由 record_start() 唤醒，随后在本任务内采集 PCM。 */
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);

        app_ai_voice_set_state(APP_AI_STATE_RECORDING);
        app_ai_voice_record_result_t record = app_ai_voice_record_to_opus_buffer();
        app_business_audio_session_end();
        APP_LOGI(TAG,
                 "AI 录音采集结束，已释放音频会话, samples=%u",
                 (unsigned int)record.pcm_samples);
        if (record.canceled || app_ai_voice_is_cancel_requested()) {
            (void)app_ui_set_ai_waiting(0);
            (void)app_ui_set_ai_message(UI_TEXT_AI_IDLE);
            app_ai_voice_clear_reply_state();
            app_ai_voice_set_current_session(NULL);
            app_ai_voice_set_state(APP_AI_STATE_CANCELED);
            s_ai_cancel_requested = 0;
            continue;
        }
        if (record.frame_count == 0u || record.pcm_samples == 0u || record.encode_ok == 0u) {
            (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
            continue;
        }

        if (service_network_is_ready() != 1) {
            (void)app_ui_set_ai_message(UI_TEXT_AI_NO_NETWORK);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
            continue;
        }

        /* AOP1 头已回填；上传与回复下载走独立持久 WebSocket，不占用音频会话。 */
        uint32_t request_audio_len = record.container_bytes;
        if (request_audio_len > APP_AI_AOP1_HEADER_LEN &&
            request_audio_len <= APP_BUSINESS_AI_OPUS_BUF_BYTES) {
            app_ai_voice_clear_reply_state();
            char request_id[APP_AI_WS_REQUEST_ID_BYTES];
            char request_sha256[APP_AI_WS_SHA256_TEXT_BYTES];
            request_id[0] = '\0';
            request_sha256[0] = '\0';
            app_ai_ws_voice_result_t result;
            memset(&result, 0, sizeof(result));
            int ret = app_ai_voice_prepare_request_identity(s_ai_request_audio_buf,
                                                            request_audio_len,
                                                            request_id,
                                                            request_sha256);
            if (ret == 0) {
                app_ai_voice_set_state(APP_AI_STATE_UPLOADING);
                if (app_ai_voice_is_cancel_requested()) {
                    ret = APP_AI_WS_ERR_CANCELLED;
                } else {
                    ret = app_ai_ws_voice_request(request_id,
                                                  s_ai_request_audio_buf,
                                                  request_audio_len,
                                                  request_sha256,
                                                  s_ai_reply_chunk_buf,
                                                  APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES,
                                                  &result);
                }
            }
            if (ret == APP_AI_WS_ERR_CANCELLED || app_ai_voice_is_cancel_requested()) {
                (void)app_ui_set_ai_waiting(0);
                (void)app_ui_set_ai_message(UI_TEXT_AI_IDLE);
                app_ai_voice_clear_reply_state();
                app_ai_voice_set_current_session(NULL);
                app_ai_voice_set_state(APP_AI_STATE_CANCELED);
                ret = APP_AI_CANCELED_RET;
            } else if (ret != 0) {
                APP_LOGW(TAG,
                         "AI WebSocket 问答失败, request_id=%s, ret=%d",
                         request_id,
                         ret);
                (void)app_ui_set_ai_waiting(0);
                (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
                (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                app_ai_voice_set_state(APP_AI_STATE_FAILED);
            } else {
                app_ai_voice_set_current_session(result.session);
                (void)app_ui_set_ai_waiting(0);
                if (result.answer_text[0] != '\0') {
                    (void)app_ui_set_ai_answer_text(result.answer_text);
                }

                if (result.no_speech) {
                    if (result.answer_text[0] == '\0') {
                        (void)app_ui_set_ai_answer_text("我没有听清，请再说一遍。");
                    }
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
                } else if (result.reply_bytes > 0u &&
                           result.reply_bytes <= APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES) {
                    app_ai_voice_store_reply_state(result.session, result.reply_bytes);
                    app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
#if AUTO_PLAY_REPLY_AUDIO
                    if (app_business_audio_session_try_begin() != 0) {
                        APP_LOGW(TAG, "AI 自动播放跳过: 音频会话占用中");
                        ret = 0;
                    } else {
                        app_ai_voice_set_state(APP_AI_STATE_PLAYING_AUDIO);
                        ret = app_ai_voice_play_rop1(s_ai_reply_chunk_buf, result.reply_bytes);
                        if (ret == 0) {
                            APP_LOGI("AI-UI", "play done");
                            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                        } else if (ret == APP_AI_PLAY_STOPPED_RET) {
                            APP_LOGI("AI-UI", "play stopped");
                            s_reply_stop_requested = 0;
                            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                            app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
                        } else if (ret == APP_AI_CANCELED_RET) {
                            (void)app_ui_set_ai_waiting(0);
                            (void)app_ui_set_ai_message(UI_TEXT_AI_IDLE);
                            app_ai_voice_clear_reply_state();
                            app_ai_voice_set_state(APP_AI_STATE_CANCELED);
                        } else {
                            (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                            app_ai_voice_set_state(APP_AI_STATE_FAILED);
                        }
                        app_business_audio_session_end();
                    }
#else
                    ret = 0;
#endif
                } else if (result.answer_text[0] != '\0') {
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
                    app_ai_voice_set_state(APP_AI_STATE_IDLE);
                } else {
                    ret = -10;
                    (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                    app_ai_voice_set_state(APP_AI_STATE_FAILED);
                }
            }

            if (ret != 0 &&
                ret != APP_AI_CANCELED_RET &&
                ret != APP_AI_TEXT_ONLY_RET) {
                APP_LOGW(TAG, "AI WebSocket 问答结束异常, ret=%d", ret);
            }
        } else {
            APP_LOGW(TAG,
                     "AI 录音 AOP1 长度无效或超限, len=%u",
                     (unsigned int)request_audio_len);
            (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
        }

        /* 录音结束时已经释放音频会话；网络与文本等待阶段不能继续占用 PTT。 */
        if (app_ai_voice_is_cancel_requested()) {
            app_ai_voice_set_current_session(NULL);
            s_ai_cancel_requested = 0;
        } else if (s_ai_state != APP_AI_STATE_AUDIO_READY) {
            app_ai_voice_set_current_session(NULL);
            app_ai_voice_set_state(APP_AI_STATE_IDLE);
        }
    }
}

/* ==========================================================================
 * 公开接口
 * ========================================================================== */

/**
 * @brief 启动 AI 语音问答模块（开机时由 app_business_start 调用）。
 *
 * 初始化共享 Opus encoder，分配 AOP1 缓冲区并创建 biz_ai 任务，设置 s_started = 1。
 * 之后 UI 长按 AI 按钮时即可触发录音流程。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_ai_voice_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = app_audio_opus_encoder_init();
    if (ret != 0) {
        APP_LOGE(TAG, "AI Opus encoder 初始化失败, ret=%d", ret);
        return -1;
    }

    s_ai_request_audio_buf =
        (uint8_t *)osal_heap_alloc_external(APP_BUSINESS_AI_OPUS_BUF_BYTES);
    if (s_ai_request_audio_buf == NULL) {
        APP_LOGE(TAG,
                 "AI AOP1 缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_BUSINESS_AI_OPUS_BUF_BYTES,
                 (unsigned int)osal_heap_get_external_free_size());
        return -2;
    }

    APP_LOGI(TAG,
             "AI AOP1 缓存已分配到 PSRAM, bytes=%u, cbr_60s_bytes=%u",
             (unsigned int)APP_BUSINESS_AI_OPUS_BUF_BYTES,
             (unsigned int)APP_AI_OPUS_CBR_MAX_CONTAINER_BYTES);

    s_ai_reply_chunk_buf =
        (uint8_t *)osal_heap_alloc_external(APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES);
    if (s_ai_reply_chunk_buf == NULL) {
        APP_LOGE(TAG,
                 "AI ROP1 回复缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES,
                 (unsigned int)osal_heap_get_external_free_size());
        osal_heap_free(s_ai_request_audio_buf);
        s_ai_request_audio_buf = NULL;
        return -3;
    }

    APP_LOGI(TAG,
             "AI ROP1 回复缓存已分配到 PSRAM, bytes=%u",
             (unsigned int)APP_BUSINESS_AI_REPLY_OPUS_MAX_BYTES);

    TaskHandle_t ai_handle = NULL;
    BaseType_t task_ret = xTaskCreateWithCaps(app_ai_voice_task,
                                              "biz_ai",
                                              APP_AI_TASK_STACK,
                                              NULL,
                                              5u,
                                              &ai_handle,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        APP_LOGE(TAG,
                 "AI 语音任务启动失败, ret=%d, psram_free=%u, internal_free=%u, internal_largest=%u",
                 (int)task_ret,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        osal_heap_free(s_ai_reply_chunk_buf);
        s_ai_reply_chunk_buf = NULL;
        osal_heap_free(s_ai_request_audio_buf);
        s_ai_request_audio_buf = NULL;
        return -4;
    }

    s_ai_task = (osal_task_t)ai_handle;
    s_started = 1;
    return 0;
}

/**
 * @brief 开始 AI 问答录音（UI 长按 AI 按钮时调用）。
 *
 * ## 执行流程
 * 1. 检查模块是否已启动（s_started）
 * 2. 检查音频会话是否被 PTT 占用（app_business_audio_session_is_busy）
 * 3. 非阻塞尝试抢占音频会话锁（app_business_audio_session_try_begin）
 * 4. 设置 s_ai_recording = 1
 * 5. notify_give(s_ai_task) → 唤醒 AI 任务开始循环读取 PCM
 *
 * ## 互斥保护
 * 若当前 PTT 已占用音频会话（s_audio_session_busy == 1），本次请求会被静默忽略。
 * 同理，若 AI 已占用，PTT 的 try_begin 也会失败。
 */
void app_ai_voice_record_start(void)
{
    if (!s_started) {
        APP_LOGW(TAG, "AI 录音开始忽略: 模块未启动");
        return;
    }
    if (s_ai_state == APP_AI_STATE_CANCELING) {
        APP_LOGW(TAG, "AI 录音开始忽略: 正在取消当前会话");
        return;
    }
    if (!app_ai_voice_can_start_recording_now()) {
        APP_LOGW(TAG, "AI 录音开始忽略: 当前会话处理中, state=%d", (int)s_ai_state);
        return;
    }
    if (s_ai_recording) {
        APP_LOGW(TAG, "AI 录音开始忽略: 已在录音中");
        return;
    }
    if (s_reply_play_busy) {
        APP_LOGW(TAG, "AI 录音开始忽略: 回复语音播放中");
        return;
    }
    if (app_business_audio_session_is_busy()) {
        APP_LOGW(TAG, "AI 录音开始忽略: 音频会话占用中");
        return;
    }
    if (app_business_audio_session_try_begin() != 0) {
        APP_LOGW(TAG, "AI 录音开始忽略: 音频会话抢占失败");
        return;
    }

    s_ai_cancel_requested = 0;
    app_ai_voice_set_current_session(NULL);
    app_ai_voice_clear_reply_state();
    app_ai_voice_set_state(APP_AI_STATE_RECORDING);
    s_ai_recording = 1;
    if (s_ai_task != NULL) {
        (void)osal_task_notify_give(s_ai_task);
    }
}

/**
 * @brief 停止 AI 问答录音（UI 松开 AI 按钮时调用）。
 *
 * ## 执行流程
 * 1. 检查 s_ai_recording 是否为 1（防止重复停止）
 * 2. 清零 s_ai_recording
 * 3. biz_ai 任务在下一次 read 超时或读完一帧后退出采集循环并开始处理
 *
 * ## 调度细节
 * biz_ai 任务被唤醒后执行以下流程（详见 app_ai_voice_task）：
 * 获取录音 → 构造 AOP1 → WebSocket 上传 → 缓存 ROP1 → 用户按需播放
 *
 * 注意：录音停止后 UI 不等待网络响应，biz_ai 任务在后台处理上传和回复下载。
 * 用户松手后 UI 立即恢复，不需要等待 AI 服务器返回。
 */
void app_ai_voice_record_stop(void)
{
    if (!s_ai_recording) {
        return;
    }
    s_ai_recording = 0;
}

void app_ai_voice_request_reply_play(void)
{
    APP_LOGI("AI-UI", "play button clicked");

    if (!s_started || !s_reply_audio_ready || s_reply_session[0] == '\0' || s_reply_wav_size == 0u) {
        APP_LOGW("AI-UI", "play button disabled, audio not ready");
        return;
    }
    if (s_reply_play_busy) {
        return;
    }

    s_reply_stop_requested = 0;
    s_reply_play_busy = 1;
    TaskHandle_t play_handle = NULL;
    if (xTaskCreateWithCaps(app_ai_voice_play_request_task,
                            "ai_reply_play",
                            APP_AI_REPLY_PLAY_TASK_STACK,
                            NULL,
                            5u,
                            &play_handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_reply_play_busy = 0;
        APP_LOGW("AI-UI", "play task create failed");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
    }
}

void app_ai_voice_request_reply_stop(void)
{
    if (!s_started || !s_reply_play_busy) {
        return;
    }

    APP_LOGI("AI-UI", "stop playback requested");
    /* ROP1 已完整下载并校验到 PSRAM；此处只停止本地解码播放。 */
    s_reply_stop_requested = 1;
    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
}

static int app_ai_voice_state_has_backend_work(app_ai_voice_state_t state)
{
    return state == APP_AI_STATE_UPLOADING ||
           state == APP_AI_STATE_FINISHING ||
           state == APP_AI_STATE_WAITING_TEXT ||
           state == APP_AI_STATE_WAITING_AUDIO ||
           state == APP_AI_STATE_DOWNLOADING_AUDIO;
}

esp_err_t app_ai_voice_cancel_current(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    app_ai_voice_state_t old_state = s_ai_state;
    if (old_state == APP_AI_STATE_IDLE ||
        old_state == APP_AI_STATE_CANCELED ||
        old_state == APP_AI_STATE_FAILED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (old_state == APP_AI_STATE_CANCELING) {
        return ESP_OK;
    }

    APP_LOGI(CANCEL_TAG, "user requested cancel, state=%d", (int)old_state);
    s_ai_cancel_requested = 1;
    app_ai_voice_set_state(APP_AI_STATE_CANCELING);

    if (s_ai_recording) {
        APP_LOGI(CANCEL_TAG, "cancel during recording");
        s_ai_recording = 0;
    }
    if (s_reply_play_busy) {
        APP_LOGI(CANCEL_TAG, "cancel during playback");
    }

    if (app_ai_voice_state_has_backend_work(old_state)) {
        char session[64];
        strncpy(session, s_ai_current_session, sizeof(session) - 1u);
        session[sizeof(session) - 1u] = '\0';
        /* WebSocket 活动命令持有 request_id/session，早期 session 为空也必须中止。 */
        app_ai_voice_request_backend_cancel_async(session);
    }

    app_ai_voice_clear_reply_state();
    (void)app_ui_set_ai_waiting(0);
    (void)app_ui_set_ai_message(UI_TEXT_AI_IDLE);

    if (old_state == APP_AI_STATE_AUDIO_READY) {
        app_ai_voice_set_current_session(NULL);
        s_ai_cancel_requested = 0;
        app_ai_voice_set_state(APP_AI_STATE_CANCELED);
    }

    return ESP_OK;
}

int app_ai_voice_is_busy(void)
{
    app_ai_voice_state_t state = s_ai_state;
    return (s_ai_recording != 0 ||
            s_reply_play_busy != 0 ||
            (state != APP_AI_STATE_IDLE &&
             state != APP_AI_STATE_AUDIO_READY &&
             state != APP_AI_STATE_CANCELED &&
             state != APP_AI_STATE_FAILED)) ? 1 : 0;
}
