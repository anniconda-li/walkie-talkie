/**
 * @file app_ai_voice.c
 * @brief AI 语音问答业务——录音→上传→播放全流程。
 *
 * ## 业务流程概述
 * 1. 用户长按 UI 上的 AI 按钮 → UI 回调触发 app_ai_voice_record_start()
 * 2. 抢占音频会话互斥锁（与 PTT 互斥）→ 唤醒 AI 任务采集 PCM
 * 3. AI 任务循环读取 service_audio PCM，直接写入请求 WAV 缓冲区
 * 4. 用户松手 → UI 回调触发 app_ai_voice_record_stop()
 * 5. 停止录音 → biz_ai 任务退出采集循环
 * 6. biz_ai 任务：写 WAV 文件头 → 分片 POST 到 AI 服务器
 * 7. 分片拉取服务器返回的 WAV 响应 → 边解析边播放 AI 回答
 * 8. 释放音频会话互斥锁，等待下一次唤醒
 *
 * ## 任务调度关系
 * - UI 线程（LVGL）→ 调用 record_start/stop → 设置标志位 + notify 目标任务
 * - biz_ai（优先级5）→ 等待 notify，采集录音、处理 WAV 上传和回答播放
 */
#include "app_ai_voice.h"

#include "app_business.h"
#include "app_config.h"
#include "app_ui.h"
#include "osal_heap.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include "esp_err.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ai_voice";
static const char *CANCEL_TAG = "APP_AI_CANCEL";

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief AI 处理任务句柄，入口为 app_ai_voice_task()，优先级 5，栈 8192 字节。 */
static osal_task_t s_ai_task = NULL;

/** @brief AI 模块是否已通过 app_ai_voice_start() 启动。
 *  启动后 task 和 WAV buffer 都已就绪，可以响应录音请求。 */
static volatile int s_started = 0;

/** @brief 当前是否处于 AI 录音进行中。
 *  由 record_start 置 1，record_stop 清 0。
 *  record_stop 会检查此标志，防止重复停止。 */
static volatile int s_ai_recording = 0;

/** @brief AI 请求 WAV 缓冲区，分配到 PSRAM，保存 60 秒内请求 WAV。 */
static uint8_t *s_ai_wav_buf = NULL;

/** @brief AI 回复分片缓冲区，播放完当前分片后直接复用，不缓存完整回复。 */
static uint8_t *s_ai_reply_chunk_buf = NULL;

/** @brief AI 分片协议 JSON 响应临时缓冲，避免覆盖正在上传的 WAV 数据。 */
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
} app_ai_voice_result_info_t;

static app_ai_voice_result_info_t s_ai_result_info;
static char s_reply_session[64];
static uint32_t s_reply_wav_size = 0u;
static volatile int s_reply_audio_ready = 0;
static volatile int s_reply_play_busy = 0;

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
static volatile int s_ai_cancel_task_busy = 0;
static char s_ai_cancel_task_session[64];

#define APP_AI_CANCELED_RET                 (-900)
#define APP_AI_CANCEL_HTTP_TIMEOUT_MS       5000u

static int app_ai_voice_is_cancel_requested(void)
{
    return s_ai_cancel_requested != 0;
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

/* ==========================================================================
 * WAV 格式辅助函数
 * ========================================================================== */

/**
 * @brief 向缓冲区写入 little-endian uint16 字段。
 *
 * WAV 文件格式要求所有多字节字段为小端序，
 * 这在 ESP32（小端 CPU）上等同于直接写入内存，但显式拆分保证跨平台正确性。
 */
static void app_ai_voice_wav_write_u16(uint8_t *buf, uint16_t value)
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
static void app_ai_voice_wav_write_u32(uint8_t *buf, uint32_t value)
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

/**
 * @brief 写入标准 PCM WAV 文件头（44 字节）。
 *
 * 结构：RIFF(4) + 文件总长(4) + "WAVEfmt "(8) + fmt chunk(16+4) + "data"(4) + data 长度(4)
 *
 * 音频参数固定为本机配置：16000Hz、单声道、16bit PCM。
 *
 * @param buf     输出缓冲区，至少 44 字节。
 * @param pcm_bytes PCM 数据的字节数（不含头）。
 */
static void app_ai_voice_wav_write_header(uint8_t *buf, uint32_t pcm_bytes)
{
    /* 第一版只生成最基础的 PCM WAV：RIFF + fmt + data 三段。 */
    uint32_t byte_rate = APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint16_t block_align = APP_BUSINESS_AUDIO_CHANNELS * (APP_BUSINESS_AUDIO_BITS / 8u);
    uint32_t riff_size = 36u + pcm_bytes;

    memcpy(&buf[0], "RIFF", 4u);
    app_ai_voice_wav_write_u32(&buf[4], riff_size);
    memcpy(&buf[8], "WAVEfmt ", 8u);
    app_ai_voice_wav_write_u32(&buf[16], 16u);
    app_ai_voice_wav_write_u16(&buf[20], 1u);                     /* PCM = 1 */
    app_ai_voice_wav_write_u16(&buf[22], APP_BUSINESS_AUDIO_CHANNELS);
    app_ai_voice_wav_write_u32(&buf[24], APP_BUSINESS_AUDIO_SAMPLE_RATE);
    app_ai_voice_wav_write_u32(&buf[28], byte_rate);
    app_ai_voice_wav_write_u16(&buf[32], block_align);
    app_ai_voice_wav_write_u16(&buf[34], APP_BUSINESS_AUDIO_BITS);
    memcpy(&buf[36], "data", 4u);
    app_ai_voice_wav_write_u32(&buf[40], pcm_bytes);
}

/** @brief WAV fmt chunk 中当前解析器至少需要的 PCM 格式字段长度。 */
#define APP_AI_WAV_FMT_MIN_BYTES      16u
/** @brief AI 回复播放时单次提交给 service_audio 的最大 PCM 样本数。 */
#define APP_AI_PLAY_CHUNK_SAMPLES     256u
/** @brief AI 回复下载和播放之间的 PCM 预缓冲块样本数。 */
#define APP_AI_REPLY_PCM_BLOCK_SAMPLES 512u
/** @brief AI 回复 PCM 预缓冲块数量，约 512ms 音频。 */
#define APP_AI_REPLY_PCM_BLOCK_COUNT   16u
/** @brief AI 回复播放启动前的预缓冲块数，降低 HTTP 分片间隙导致的卡顿。 */
#define APP_AI_REPLY_PLAY_START_BLOCKS 8u
/** @brief AI 回复播放启动前最多等待预缓冲的时间。 */
#define APP_AI_REPLY_PLAY_START_WAIT_MS 600u
/** @brief AI 回复下载任务栈大小。 */
#define APP_AI_REPLY_FETCH_TASK_STACK  4096u
/** @brief AI 录音任务单次读取超时时间，单位 ms。 */
#define APP_AI_RECORD_READ_TIMEOUT_MS 30u

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
        if (app_ai_voice_is_cancel_requested()) {
            return APP_AI_CANCELED_RET;
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
        if (app_ai_voice_is_cancel_requested()) {
            (void)osal_queue_send(ctx->free_queue, &block_index, OSAL_WAIT_NONE);
            return APP_AI_CANCELED_RET;
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

    info->text_ready = (info->answer_text[0] != '\0' ||
                        strcmp(info->status, "text_ready") == 0 ||
                        strcmp(info->status, "audio_ready") == 0) ? 1u : 0u;
    info->audio_ready = (strcmp(info->status, "audio_ready") == 0 ||
                         app_ai_voice_json_is_true(json, len, "audio_ready") ||
                         app_ai_voice_json_is_true(json, len, "reply_wav_ready") ||
                         app_ai_voice_json_is_true(json, len, "ready")) ? 1u : 0u;
    info->audio_failed = (strcmp(info->status, "audio_failed") == 0 ||
                          strcmp(info->tts_status, "failed") == 0) ? 1u : 0u;

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

    snprintf(query, sizeof(query), "session=%s", session);
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

/** @brief 请求服务器创建一次 AI 会话。 */
static int app_ai_voice_start_session(char *session, size_t session_size)
{
    char json[96];
    char url[256];
    uint32_t resp_len = 0u;

    if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_START, "") != 0) {
        return -1;
    }

    int written = snprintf(json,
                           sizeof(json),
                           "{\"device\":\"%s\",\"language\":\"zh\"}",
                           APP_BUSINESS_DEVICE_NAME);
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
    ret = app_ai_voice_json_get_string(s_ai_resp_buf, resp_len, "session", session, session_size);
    if (ret == 0 && app_ai_voice_is_cancel_requested()) {
        app_ai_voice_set_current_session(session);
        app_ai_voice_request_backend_cancel_async(session);
        return APP_AI_CANCELED_RET;
    }
    return ret;
}

/** @brief 按固定大小上传完整请求 WAV。 */
static int app_ai_voice_upload_wav_chunks(const char *session, uint32_t wav_len)
{
    if (session == NULL || wav_len == 0u || wav_len > APP_BUSINESS_AI_REQUEST_WAV_MAX_BYTES) {
        return -1;
    }

    uint32_t offset = 0u;
    uint32_t index = 0u;
    while (offset < wav_len) {
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during upload index=%u", (unsigned int)index);
            return APP_AI_CANCELED_RET;
        }

        uint32_t chunk_len = wav_len - offset;
        if (chunk_len > APP_AI_HTTP_CHUNK_BYTES) {
            chunk_len = APP_AI_HTTP_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        snprintf(query,
                 sizeof(query),
                 "session=%s&index=%u&offset=%u&total=%u",
                 session,
                 (unsigned int)index,
                 (unsigned int)offset,
                 (unsigned int)wav_len);
        if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_UPLOAD, query) != 0) {
            return -2;
        }

        int ret = service_network_http_post(url,
                                            "application/octet-stream",
                                            &s_ai_wav_buf[offset],
                                            chunk_len,
                                            s_ai_resp_buf,
                                            sizeof(s_ai_resp_buf) - 1u,
                                            &resp_len,
                                            APP_AI_HTTP_CHUNK_TIMEOUT_MS);
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during upload index=%u", (unsigned int)index);
            return APP_AI_CANCELED_RET;
        }
        if (ret != 0) {
            APP_LOGW(TAG, "AI 上传分片失败, index=%u, offset=%u, ret=%d",
                     (unsigned int)index,
                     (unsigned int)offset,
                     ret);
            return ret;
        }

        offset += chunk_len;
        index++;
    }

    return 0;
}

/** @brief 通知服务器上传结束并开始处理。 */
static int app_ai_voice_finish_upload(const char *session)
{
    char query[128];
    char url[256];
    uint32_t resp_len = 0u;

    snprintf(query, sizeof(query), "session=%s", session);
    if (app_ai_voice_build_url(url, sizeof(url), APP_BUSINESS_HTTP_ROUTE_AI_FINISH, query) != 0) {
        return -1;
    }

    if (app_ai_voice_is_cancel_requested()) {
        return APP_AI_CANCELED_RET;
    }

    int ret = app_ai_voice_post_json(url, &resp_len);
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

        snprintf(query, sizeof(query), "session=%s", session);
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
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during reply download");
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_active_session(ctx->session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", ctx->session);
            return APP_AI_CANCELED_RET;
        }

        uint32_t chunk_len = ctx->total - offset;
        if (chunk_len > APP_AI_HTTP_CHUNK_BYTES) {
            chunk_len = APP_AI_HTTP_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        static const uint8_t empty_json[] = "{}";
        snprintf(query,
                 sizeof(query),
                 "session=%s&offset=%u&len=%u",
                 ctx->session,
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
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during reply download");
            return APP_AI_CANCELED_RET;
        }
        if (!app_ai_voice_is_active_session(ctx->session)) {
            APP_LOGW(CANCEL_TAG, "old session response ignored, session=%s", ctx->session);
            return APP_AI_CANCELED_RET;
        }
        if (ret != 0 || resp_len != chunk_len) {
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

        offset += chunk_len;
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

    while (1) {
        if (app_ai_voice_is_cancel_requested() && final_ret == 0) {
            APP_LOGI(CANCEL_TAG, "cancel during playback");
            final_ret = APP_AI_CANCELED_RET;
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
            if (app_ai_voice_is_cancel_requested()) {
                APP_LOGI(CANCEL_TAG, "cancel during playback");
                final_ret = APP_AI_CANCELED_RET;
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
    int ret = app_ai_voice_play_result_chunks(session, total);
    if (ret == 0) {
        APP_LOGI("AI-UI", "play done");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
        app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
    } else if (ret == APP_AI_CANCELED_RET) {
        (void)app_ui_set_ai_waiting(0);
        (void)app_ui_set_ai_answer_text("本次问答已取消");
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

/**
 * @brief 录制一次 AI 请求 PCM，直接追加到请求 WAV 缓冲区的 data 区。
 *
 * s_ai_wav_buf 前 44 字节预留给 WAV 头；录音阶段只写 PCM，停止后再根据
 * 实际样本数回填 WAV 头，避免 service 层再维护一份整段录音缓存。
 *
 * @return 实际录到的 int16_t 单声道样本数。
 */
static uint32_t app_ai_voice_record_to_wav_buffer(void)
{
    int16_t frame[APP_BUSINESS_FRAME_SAMPLES];
    int16_t *record_pcm = (int16_t *)&s_ai_wav_buf[APP_BUSINESS_WAV_HEADER_LEN];
    uint32_t samples_total = 0u;

    while (s_ai_recording && samples_total < APP_BUSINESS_AI_MAX_SAMPLES) {
        if (app_ai_voice_is_cancel_requested()) {
            APP_LOGI(CANCEL_TAG, "cancel during recording");
            s_ai_recording = 0;
            return 0u;
        }

        uint32_t room = APP_BUSINESS_AI_MAX_SAMPLES - samples_total;
        uint32_t request = room > APP_BUSINESS_FRAME_SAMPLES ?
                           APP_BUSINESS_FRAME_SAMPLES :
                           room;
        int read_samples = service_audio_read(frame,
                                              request,
                                              APP_AI_RECORD_READ_TIMEOUT_MS);
        if (read_samples <= 0) {
            osal_delay_ms(5u);
            continue;
        }

        memcpy(&record_pcm[samples_total],
               frame,
               (uint32_t)read_samples * sizeof(int16_t));
        samples_total += (uint32_t)read_samples;
    }

    if (samples_total >= APP_BUSINESS_AI_MAX_SAMPLES) {
        s_ai_recording = 0;
    }

    return samples_total;
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
 * 2. 循环读取 service_audio PCM，直接写入 s_ai_wav_buf 的 WAV data 区
 * 3. 写入 WAV 文件头（44 字节），封装成标准 WAV 格式
 * 5. 创建服务器 session，按 32KB 分片上传请求 WAV
 * 6. finish 后轮询 result_info，拿到回复 WAV 总长度
 * 7. 按 32KB 拉取 result_chunk，边解析 WAV 边播放 PCM，播放后丢弃分片
 * 8. 调用 app_business_audio_session_end() 释放音频会话锁
 * 9. 回到步骤 1，等待下一次录音完成
 *
 * ## 错误处理
 * - 任何步骤失败都通过 app_business_audio_session_end() 释放锁
 * - 不会因为单次失败导致锁泄漏或任务死锁
 *
 * ## 内存策略
 * - 请求 WAV 使用 s_ai_wav_buf；录音 PCM 直接写到 WAV data 区，不经过 service 缓冲
 * - 回复 WAV 只使用单个 HTTP 分片缓冲，边播边丢弃
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
        uint32_t samples_total = app_ai_voice_record_to_wav_buffer();
        if (app_ai_voice_is_cancel_requested()) {
            (void)app_ui_set_ai_waiting(0);
            (void)app_ui_set_ai_answer_text("本次问答已取消");
            app_ai_voice_clear_reply_state();
            app_ai_voice_set_current_session(NULL);
            app_ai_voice_set_state(APP_AI_STATE_CANCELED);
            s_ai_cancel_requested = 0;
            app_business_audio_session_end();
            continue;
        }
        if (samples_total == 0u) {
            (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
            app_business_audio_session_end();
            continue;
        }

        if (service_network_is_ready() != 1) {
            (void)app_ui_set_ai_message(UI_TEXT_AI_NO_NETWORK);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_HIDDEN);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
            app_business_audio_session_end();
            continue;
        }

        /* 构造请求 WAV 文件：录音 PCM 已在 data 区，这里只回填 WAV 头。 */
        uint32_t pcm_bytes = samples_total * sizeof(int16_t);
        uint32_t wav_len = APP_BUSINESS_WAV_HEADER_LEN + pcm_bytes;
        if (pcm_bytes > 0u && wav_len <= APP_BUSINESS_AI_REQUEST_WAV_MAX_BYTES) {
            app_ai_voice_wav_write_header(s_ai_wav_buf, pcm_bytes);
            app_ai_voice_clear_reply_state();

            char session[64];
            session[0] = '\0';
            app_ai_voice_set_state(APP_AI_STATE_UPLOADING);
            int ret = app_ai_voice_start_session(session, sizeof(session));
            if (ret == 0) {
                app_ai_voice_set_current_session(session);
                ret = app_ai_voice_upload_wav_chunks(session, wav_len);
            }
            if (ret == 0) {
                app_ai_voice_set_state(APP_AI_STATE_FINISHING);
                ret = app_ai_voice_finish_upload(session);
            }
            if (ret != 0) {
                if (ret == APP_AI_CANCELED_RET) {
                    (void)app_ui_set_ai_waiting(0);
                    (void)app_ui_set_ai_answer_text("本次问答已取消");
                    app_ai_voice_clear_reply_state();
                    app_ai_voice_set_state(APP_AI_STATE_CANCELED);
                } else {
                    (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                    app_ai_voice_set_state(APP_AI_STATE_FAILED);
                }
            }
            uint32_t reply_len = 0u;
            if (ret == 0) {
                app_ai_voice_set_state(APP_AI_STATE_WAITING_TEXT);
                ret = app_ai_voice_wait_result_info(session, &reply_len);
                if (ret == APP_AI_CANCELED_RET) {
                    (void)app_ui_set_ai_waiting(0);
                    (void)app_ui_set_ai_answer_text("本次问答已取消");
                    app_ai_voice_clear_reply_state();
                    app_ai_voice_set_state(APP_AI_STATE_CANCELED);
                } else if (ret != 0 && ret != -4 && ret != -5) {
                    (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                    app_ai_voice_set_state(APP_AI_STATE_FAILED);
                }
            }
            if (ret == 0) {
                if (reply_len > APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES) {
                    APP_LOGW(TAG,
                             "AI 回复 WAV 超限, len=%u, max=%u",
                             (unsigned int)reply_len,
                             (unsigned int)APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES);
                    ret = -10;
                    (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                    (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
                    app_ai_voice_set_state(APP_AI_STATE_FAILED);
                } else {
                    app_ai_voice_set_state(APP_AI_STATE_AUDIO_READY);
#if AUTO_PLAY_REPLY_AUDIO
                    app_ai_voice_set_state(APP_AI_STATE_DOWNLOADING_AUDIO);
                    ret = app_ai_voice_play_result_chunks(session, reply_len);
                    if (ret == 0) {
                        APP_LOGI("AI-UI", "play done");
                        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                    } else if (ret == APP_AI_CANCELED_RET) {
                        (void)app_ui_set_ai_waiting(0);
                        (void)app_ui_set_ai_answer_text("本次问答已取消");
                        app_ai_voice_clear_reply_state();
                        app_ai_voice_set_state(APP_AI_STATE_CANCELED);
                    } else {
                        (void)app_ui_set_ai_message(UI_TEXT_AI_REPLY_FAILED);
                        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
                        app_ai_voice_set_state(APP_AI_STATE_FAILED);
                    }
#else
                    ret = 0;
#endif
                }
            }

            if (ret != 0) {
                APP_LOGW(TAG, "AI 分片问答失败, ret=%d", ret);
            }
        } else {
            APP_LOGW(TAG, "AI 录音 WAV 长度无效或超限, wav_len=%u", (unsigned int)wav_len);
            (void)app_ui_set_ai_message(UI_TEXT_AI_QUESTION_FAILED);
            (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_FAILED);
            app_ai_voice_set_state(APP_AI_STATE_FAILED);
        }

        /* 步骤 8：无论成功失败都释放音频会话锁 */
        app_business_audio_session_end();
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
 * 直接分配 WAV 缓冲区并创建 biz_ai 任务，设置 s_started = 1。
 * 之后 UI 长按 AI 按钮时即可触发录音流程。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_ai_voice_start(void)
{
    if (s_started) {
        return 0;
    }

    s_ai_wav_buf = (uint8_t *)osal_heap_alloc_external(APP_BUSINESS_AI_WAV_BUF_BYTES);
    if (s_ai_wav_buf == NULL) {
        APP_LOGE(TAG,
                 "AI WAV 缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_BUSINESS_AI_WAV_BUF_BYTES,
                 (unsigned int)osal_heap_get_external_free_size());
        return -1;
    }

    APP_LOGI(TAG,
             "AI WAV 缓存已分配到 PSRAM, bytes=%u",
             (unsigned int)APP_BUSINESS_AI_WAV_BUF_BYTES);

    s_ai_reply_chunk_buf = (uint8_t *)osal_heap_alloc_external(APP_AI_HTTP_CHUNK_BYTES);
    if (s_ai_reply_chunk_buf == NULL) {
        APP_LOGE(TAG,
                 "AI 回复分片缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_AI_HTTP_CHUNK_BYTES,
                 (unsigned int)osal_heap_get_external_free_size());
        osal_heap_free(s_ai_wav_buf);
        s_ai_wav_buf = NULL;
        return -2;
    }

    APP_LOGI(TAG,
             "AI 回复分片缓存已分配到 PSRAM, bytes=%u",
             (unsigned int)APP_AI_HTTP_CHUNK_BYTES);

    int ret = osal_task_create("biz_ai", app_ai_voice_task, NULL, 8192u, 5u, &s_ai_task);
    if (ret != 0) {
        APP_LOGE(TAG, "AI 语音任务启动失败, ret=%d", ret);
        osal_heap_free(s_ai_reply_chunk_buf);
        s_ai_reply_chunk_buf = NULL;
        osal_heap_free(s_ai_wav_buf);
        s_ai_wav_buf = NULL;
        return ret;
    }

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
        return;
    }
    if (s_ai_state == APP_AI_STATE_CANCELING || s_ai_cancel_task_busy) {
        return;
    }
    if (s_ai_recording || s_reply_play_busy) {
        return;
    }
    if (app_business_audio_session_is_busy()) {
        return;
    }
    if (app_business_audio_session_try_begin() != 0) {
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
 * 获取录音 → 构造 WAV → HTTP POST → 解析响应 WAV → 播放 → 释放音频会话
 *
 * 注意：录音停止后不再等待 HTTP 响应，biz_ai 任务在后台异步处理上传和播放。
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

    s_reply_play_busy = 1;
    if (osal_task_create("ai_reply_play",
                         app_ai_voice_play_request_task,
                         NULL,
                         6144u,
                         5u,
                         NULL) != 0) {
        s_reply_play_busy = 0;
        APP_LOGW("AI-UI", "play task create failed");
        (void)app_ui_set_ai_audio_button_state(UI_AI_AUDIO_BTN_READY);
    }
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

    APP_LOGI(CANCEL_TAG, "user requested cancel");
    s_ai_cancel_requested = 1;
    app_ai_voice_set_state(APP_AI_STATE_CANCELING);

    if (s_ai_recording) {
        APP_LOGI(CANCEL_TAG, "cancel during recording");
        s_ai_recording = 0;
    }
    if (s_reply_play_busy) {
        APP_LOGI(CANCEL_TAG, "cancel during playback");
    }

    char session[64];
    strncpy(session, s_ai_current_session, sizeof(session) - 1u);
    session[sizeof(session) - 1u] = '\0';
    if (session[0] != '\0') {
        app_ai_voice_request_backend_cancel_async(session);
    }

    app_ai_voice_clear_reply_state();
    (void)app_ui_set_ai_waiting(0);
    (void)app_ui_set_ai_answer_text("本次问答已取消");

    if (old_state == APP_AI_STATE_AUDIO_READY) {
        app_ai_voice_set_current_session(NULL);
        s_ai_cancel_requested = 0;
        app_ai_voice_set_state(APP_AI_STATE_CANCELED);
    }

    return ESP_OK;
}
