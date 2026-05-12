/**
 * @file app_ai_voice.c
 * @brief AI 语音问答业务——录音→上传→播放全流程。
 *
 * ## 业务流程概述
 * 1. 用户长按 UI 上的 AI 按钮 → UI 回调触发 app_ai_voice_record_start()
 * 2. 抢占音频会话互斥锁（与 PTT 互斥）→ 启动 service 层录音
 * 3. service 层录音任务循环采集麦克风 PCM 数据到 PSRAM 缓冲区
 * 4. 用户松手 → UI 回调触发 app_ai_voice_record_stop()
 * 5. 停止录音 → 通过 OSAL task notification 唤醒 biz_ai 任务
 * 6. biz_ai 任务：取出 PCM → 封装 WAV 文件头 → HTTP POST 到 AI 服务器
 * 7. 解析服务器返回的 WAV 响应 → 通过扬声器播放 AI 回答
 * 8. 释放音频会话互斥锁，等待下一次唤醒
 *
 * ## 任务调度关系
 * - UI 线程（LVGL）→ 调用 record_start/stop → 设置标志位 + notify 目标任务
 * - svc_audio_rec（优先级5）→ 录音采集循环，由 s_recording 标志控制
 * - biz_ai（优先级5）→ 等待 notify，处理 WAV 上传和回答播放
 */
#include "app_ai_voice.h"

#include "app_business.h"
#include "app_config.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include "esp_heap_caps.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "app_ai_voice";

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief AI 处理任务句柄，入口为 app_ai_voice_task()，优先级 5，栈 4096 字节。 */
static osal_task_t s_ai_task = NULL;

/** @brief AI 模块是否已通过 app_ai_voice_start() 启动。
 *  启动后 task 和 WAV buffer 都已就绪，可以响应录音请求。 */
static volatile int s_started = 0;

/** @brief 当前是否处于 AI 录音进行中。
 *  由 record_start 置 1，record_stop 清 0。
 *  record_stop 会检查此标志，防止重复停止。 */
static volatile int s_ai_recording = 0;

/** @brief AI WAV 收发复用缓冲区，分配到 PSRAM。
 *  大小 = WAV 头(44B) + 最大录音 PCM(2 秒 × 16000Hz × 2 字节 = 64000B) = 64044 字节。
 *  既是请求 body（录音 WAV），也是响应 body（AI 回答 WAV），节省内存。 */
static uint8_t *s_ai_wav_buf = NULL;

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
 * @brief 在字节缓冲区中搜索 "RIFF" 魔数。
 *
 * ML307C 4G 模块通过 AT 指令做 HTTP POST 时，响应 body 前可能混入
 * 若干 AT 前缀字节（如 "\r\n" 或 "+HTTP:" 状态行），不能直接当 WAV 解析。
 * 此函数扫描整个响应，定位 RIFF 头在缓冲区中的偏移。
 *
 * @return RIFF 偏移位置；未找到返回 -1。
 */
static int app_ai_voice_find_riff(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len < 4u) {
        return -1;
    }

    for (uint16_t i = 0; i <= (uint16_t)(len - 4u); i++) {
        if (memcmp(&buf[i], "RIFF", 4u) == 0) {
            return (int)i;
        }
    }

    return -1;
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

/**
 * @brief 解析 WAV 文件，提取 PCM 数据指针和样本数。
 *
 * 不仅校验 RIFF/WAVE 魔数，还校验音频格式是否与本机一致
 * （PCM 编码、1 声道、16000Hz、16bit），不匹配则拒绝，避免播放噪声。
 *
 * WAV 文件可能包含额外 chunk（如 LIST、fact 等），本函数按 chunk header
 * 顺序扫描，定位到 "data" chunk 后返回其内容和长度。
 *
 * @param[in]  wav     WAV 文件缓冲区。
 * @param[in]  wav_len WAV 文件总长度。
 * @param[out] pcm     提取到的 PCM 数据指针（指向 wav 内部，不复制）。
 * @param[out] samples 提取到的样本数（以 int16_t 为单位）。
 * @return 成功返回 0；失败返回负值（-1=参数无效, -2=魔数错误, -3=格式不匹配, -4=数据越界, -5=找不到 data chunk）。
 */
static int app_ai_voice_wav_parse_pcm(const uint8_t *wav,
                                      uint16_t wav_len,
                                      const int16_t **pcm,
                                      uint32_t *samples)
{
    if (wav == NULL || wav_len < APP_BUSINESS_WAV_HEADER_LEN || pcm == NULL || samples == NULL) {
        return -1;
    }
    if (memcmp(&wav[0], "RIFF", 4u) != 0 || memcmp(&wav[8], "WAVE", 4u) != 0) {
        return -2;
    }

    uint16_t audio_format = app_ai_voice_wav_read_u16(&wav[20]);
    uint16_t channels = app_ai_voice_wav_read_u16(&wav[22]);
    uint32_t sample_rate = app_ai_voice_wav_read_u32(&wav[24]);
    uint16_t bits = app_ai_voice_wav_read_u16(&wav[34]);

    /* 只接受和本机音频链路一致的 PCM 格式，避免错误播放噪声数据。 */
    if (audio_format != 1u || channels != APP_BUSINESS_AUDIO_CHANNELS ||
        sample_rate != APP_BUSINESS_AUDIO_SAMPLE_RATE || bits != APP_BUSINESS_AUDIO_BITS) {
        return -3;
    }

    uint32_t data_offset = 36u;
    while ((data_offset + 8u) <= wav_len) {
        uint32_t chunk_size = app_ai_voice_wav_read_u32(&wav[data_offset + 4u]);
        /* WAV 可能带有额外 chunk，按 chunk header 顺序查找 data。 */
        if (memcmp(&wav[data_offset], "data", 4u) == 0) {
            if ((data_offset + 8u + chunk_size) > wav_len) {
                return -4;
            }
            *pcm = (const int16_t *)&wav[data_offset + 8u];
            *samples = chunk_size / sizeof(int16_t);
            return 0;
        }
        data_offset += 8u + chunk_size + (chunk_size & 1u);
    }

    return -5;
}

/* ==========================================================================
 * AI 语音处理任务
 * ========================================================================== */

/**
 * @brief AI 语音处理后台任务入口。
 *
 * ## 执行流程（每次被 notify 唤醒一次→处理一次完整的问答）：
 *
 * 1. 阻塞等待 notify（来自 app_ai_voice_record_stop()）
 * 2. 从 service_audio 获取本次录音的 PCM 数据和长度
 * 3. 将 PCM 拷贝到 s_ai_wav_buf 中 WAV 头之后的位置
 * 4. 写入 WAV 文件头（44 字节），封装成标准 WAV 格式
 * 5. 调用 service_network_http_post_wav() 将 WAV 上传到 AI 服务器
 *    - URL: http://<server>:18080/ai/wav
 *    - 请求和响应复用同一块 s_ai_wav_buf，节省内存
 * 6. 解析服务器返回的 WAV：
 *    - 先搜索 "RIFF" 魔数（跳过 AT 前缀字节）
 *    - 校验 WAV 格式是否与本机音频链路一致
 * 7. 通过 service_audio_play() 播放 AI 回答的 PCM 音频
 * 8. 调用 app_business_audio_session_end() 释放音频会话锁
 * 9. 回到步骤 1，等待下一次录音完成
 *
 * ## 错误处理
 * - 任何步骤失败都通过 app_business_audio_session_end() 释放锁
 * - 不会因为单次失败导致锁泄漏或任务死锁
 *
 * ## 内存策略
 * - 请求 WAV 和响应 WAV 复用 s_ai_wav_buf（64044 字节，PSRAM）
 * - 录音 PCM 在 service_audio 的 s_record_buf 中，通过指针引用，不额外复制
 *
 * @param arg 未使用。
 */
static void app_ai_voice_task(void *arg)
{
    (void)arg;

    while (1) {
        /* 阻塞等待：录音完成并停止后，由 record_stop() 发通知唤醒 */
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        const int16_t *record_pcm = NULL;
        uint32_t samples_total = 0u;

        /* 步骤 1：获取录音数据（指针指向 service 层内部缓冲区） */
        int ret = service_audio_get_record_data(&record_pcm, &samples_total);
        if (ret != 0 || record_pcm == NULL || samples_total == 0u) {
            app_business_audio_session_end();
            continue;
        }

        /* 步骤 2-3：构造 WAV 文件（拷贝 PCM + 写文件头） */
        uint32_t pcm_bytes = samples_total * sizeof(int16_t);
        if (pcm_bytes > 0u) {
            int16_t *pcm = (int16_t *)&s_ai_wav_buf[APP_BUSINESS_WAV_HEADER_LEN];
            memcpy(pcm, record_pcm, pcm_bytes);
            app_ai_voice_wav_write_header(s_ai_wav_buf, pcm_bytes);

            /* 步骤 4：HTTP POST 上传 WAV 到 AI 服务器 */
            uint16_t wav_len = (uint16_t)(APP_BUSINESS_WAV_HEADER_LEN + pcm_bytes);
            uint16_t resp_len = 0u;
            ret = service_network_http_post_wav(APP_BUSINESS_AI_HTTP_URL,
                                                s_ai_wav_buf,
                                                wav_len,
                                                s_ai_wav_buf,
                                                APP_BUSINESS_AI_WAV_MAX_BYTES,
                                                &resp_len,
                                                APP_AI_HTTP_RESPONSE_TIMEOUT_MS);

            /* 步骤 5-7：解析响应 → 播放 AI 回答 */
            if (ret == 0 && resp_len > 0u) {
                const int16_t *resp_pcm = NULL;
                uint32_t resp_samples = 0u;
                /* HTTP body 经 AT 口返回时可能混入前缀字节，先定位 RIFF 再解析。 */
                int riff_pos = app_ai_voice_find_riff(s_ai_wav_buf, resp_len);
                if (riff_pos > 0) {
                    memmove(s_ai_wav_buf, &s_ai_wav_buf[riff_pos], resp_len - (uint16_t)riff_pos);
                    resp_len = (uint16_t)(resp_len - (uint16_t)riff_pos);
                }
                ret = app_ai_voice_wav_parse_pcm(s_ai_wav_buf, resp_len, &resp_pcm, &resp_samples);
                if (ret == 0 && resp_samples > 0u) {
                    /* 先标记播放开始（设置 s_playback_started 标志），再播放 PCM */
                    (void)service_audio_start_playback();
                    (void)service_audio_play(resp_pcm, resp_samples, 100u);
                } else {
                    APP_LOGW(TAG, "AI 响应 WAV 解析失败, ret=%d, len=%u", ret, (unsigned int)resp_len);
                }
            }
        }

        /* 步骤 8：无论成功失败都释放音频会话锁 */
        app_business_audio_session_end();
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

    s_ai_wav_buf = (uint8_t *)heap_caps_malloc(APP_BUSINESS_AI_WAV_MAX_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ai_wav_buf == NULL) {
        APP_LOGE(TAG,
                 "AI WAV 缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_BUSINESS_AI_WAV_MAX_BYTES,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return -1;
    }

    APP_LOGI(TAG,
             "AI WAV 缓存已分配到 PSRAM, bytes=%u",
             (unsigned int)APP_BUSINESS_AI_WAV_MAX_BYTES);

    int ret = osal_task_create("biz_ai", app_ai_voice_task, NULL, 4096u, 5u, &s_ai_task);
    if (ret != 0) {
        APP_LOGE(TAG, "AI 语音任务启动失败, ret=%d", ret);
        heap_caps_free(s_ai_wav_buf);
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
 * 4. 清空上次录音数据，启动 service 层录音
 * 5. 设置 s_ai_recording = 1
 *
 * ## 调度细节
 * service_audio_start_record() 内部会：
 * - 清零 s_record_samples
 * - 设置 s_recording = 1u
 * - notify_give(s_record_task) → 唤醒 svc_audio_rec 开始采集循环
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
    if (app_business_audio_session_is_busy()) {
        return;
    }
    if (app_business_audio_session_try_begin() != 0) {
        return;
    }
    if (service_audio_start_record() != 0) {
        app_business_audio_session_end();
        return;
    }

    s_ai_recording = 1;
}

/**
 * @brief 停止 AI 问答录音（UI 松开 AI 按钮时调用）。
 *
 * ## 执行流程
 * 1. 检查 s_ai_recording 是否为 1（防止重复停止）
 * 2. 清零 s_ai_recording
 * 3. 调用 service_audio_stop_record()：
 *    - 清零 s_recording → 录音任务在循环中检测到此标志后退出内层循环
 *    - 等待 svc_audio_rec 任务退出（最多 150ms）
 * 4. notify_give(s_ai_task) → 唤醒 biz_ai 任务开始处理
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
    (void)service_audio_stop_record();
    if (s_ai_task != NULL) {
        (void)osal_task_notify_give(s_ai_task);
    }
}
