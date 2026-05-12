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
 * 6. biz_ai 任务：取出 PCM → 封装 WAV 文件头 → 分片 POST 到 AI 服务器
 * 7. 分片拉取服务器返回的 WAV 响应 → 通过扬声器播放 AI 回答
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
#include <stdio.h>
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
 *  上传阶段保存 60 秒内请求 WAV；上传结束后被 120 秒内回复 WAV 覆盖。 */
static uint8_t *s_ai_wav_buf = NULL;

/** @brief AI 分片协议 JSON 响应临时缓冲，避免覆盖正在上传的 WAV 数据。 */
static uint8_t s_ai_resp_buf[512];

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
static int app_ai_voice_find_riff(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len < 4u) {
        return -1;
    }

    for (uint32_t i = 0; i <= (len - 4u); i++) {
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
                                      uint32_t wav_len,
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

/**
 * @brief 构造带 query 参数的 AI URL。
 *
 * APP_BUSINESS_AI_HTTP_URL 可能已经带有 language=zh 等参数，因此这里根据
 * base URL 中是否存在 '?' 自动选择追加 '?' 或 '&'。
 */
static int app_ai_voice_build_url(char *out, size_t out_size, const char *query)
{
    if (out == NULL || out_size == 0u || query == NULL) {
        return -1;
    }

    const char *sep = strchr(APP_BUSINESS_AI_HTTP_URL, '?') != NULL ? "&" : "?";
    int written = snprintf(out, out_size, "%s%s%s", APP_BUSINESS_AI_HTTP_URL, sep, query);
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

/** @brief POST 一个小 JSON 请求并把响应放入 s_ai_resp_buf。 */
static int app_ai_voice_post_json(const char *url, uint32_t *resp_len)
{
    static const uint8_t empty_json[] = "{}";

    return service_network_http_post(url,
                                     "application/json",
                                     empty_json,
                                     sizeof(empty_json) - 1u,
                                     s_ai_resp_buf,
                                     sizeof(s_ai_resp_buf) - 1u,
                                     resp_len,
                                     APP_AI_HTTP_CHUNK_TIMEOUT_MS);
}

/** @brief 请求服务器创建一次 AI 会话。 */
static int app_ai_voice_start_session(char *session, size_t session_size)
{
    char query[96];
    char url[256];
    uint32_t resp_len = 0u;

    snprintf(query, sizeof(query), "op=start&device=%s", APP_BUSINESS_DEVICE_NAME);
    if (app_ai_voice_build_url(url, sizeof(url), query) != 0) {
        return -1;
    }

    int ret = app_ai_voice_post_json(url, &resp_len);
    if (ret != 0) {
        return ret;
    }
    s_ai_resp_buf[resp_len < sizeof(s_ai_resp_buf) ? resp_len : (sizeof(s_ai_resp_buf) - 1u)] = '\0';
    return app_ai_voice_json_get_string(s_ai_resp_buf, resp_len, "session", session, session_size);
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
        uint32_t chunk_len = wav_len - offset;
        if (chunk_len > APP_AI_HTTP_CHUNK_BYTES) {
            chunk_len = APP_AI_HTTP_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        snprintf(query,
                 sizeof(query),
                 "op=upload&session=%s&index=%u&offset=%u&total=%u",
                 session,
                 (unsigned int)index,
                 (unsigned int)offset,
                 (unsigned int)wav_len);
        if (app_ai_voice_build_url(url, sizeof(url), query) != 0) {
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

    snprintf(query, sizeof(query), "op=finish&session=%s", session);
    if (app_ai_voice_build_url(url, sizeof(url), query) != 0) {
        return -1;
    }

    return app_ai_voice_post_json(url, &resp_len);
}

/** @brief 轮询服务器，直到 AI 回复 WAV 准备好并返回总长度。 */
static int app_ai_voice_wait_result_info(const char *session, uint32_t *total)
{
    if (session == NULL || total == NULL) {
        return -1;
    }

    uint32_t start = osal_get_tick_ms();
    while ((osal_get_tick_ms() - start) < APP_AI_PROCESS_TIMEOUT_MS) {
        char query[128];
        char url[256];
        uint32_t resp_len = 0u;

        snprintf(query, sizeof(query), "op=result_info&session=%s", session);
        if (app_ai_voice_build_url(url, sizeof(url), query) != 0) {
            return -2;
        }

        int ret = app_ai_voice_post_json(url, &resp_len);
        if (ret == 0) {
            s_ai_resp_buf[resp_len < sizeof(s_ai_resp_buf) ? resp_len : (sizeof(s_ai_resp_buf) - 1u)] = '\0';
            if (app_ai_voice_json_is_true(s_ai_resp_buf, resp_len, "ready")) {
                ret = app_ai_voice_json_get_u32(s_ai_resp_buf, resp_len, "total", total);
                if (ret == 0) {
                    return 0;
                }
                return ret;
            }
        }

        osal_delay_ms(APP_AI_RESULT_POLL_MS);
    }

    return -3;
}

/** @brief 按 offset/len 拉取回复 WAV 分片并写入复用大缓冲。 */
static int app_ai_voice_download_result_chunks(const char *session, uint32_t total)
{
    if (session == NULL || total < APP_BUSINESS_WAV_HEADER_LEN ||
        total > APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES) {
        return -1;
    }

    uint32_t offset = 0u;
    while (offset < total) {
        uint32_t chunk_len = total - offset;
        if (chunk_len > APP_AI_HTTP_CHUNK_BYTES) {
            chunk_len = APP_AI_HTTP_CHUNK_BYTES;
        }

        char query[192];
        char url[320];
        uint32_t resp_len = 0u;
        static const uint8_t empty_json[] = "{}";
        snprintf(query,
                 sizeof(query),
                 "op=result_chunk&session=%s&offset=%u&len=%u",
                 session,
                 (unsigned int)offset,
                 (unsigned int)chunk_len);
        if (app_ai_voice_build_url(url, sizeof(url), query) != 0) {
            return -2;
        }

        int ret = service_network_http_post(url,
                                            "application/json",
                                            empty_json,
                                            sizeof(empty_json) - 1u,
                                            &s_ai_wav_buf[offset],
                                            chunk_len,
                                            &resp_len,
                                            APP_AI_HTTP_CHUNK_TIMEOUT_MS);
        if (ret != 0 || resp_len != chunk_len) {
            APP_LOGW(TAG,
                     "AI 回复分片下载失败, offset=%u, expect=%u, got=%u, ret=%d",
                     (unsigned int)offset,
                     (unsigned int)chunk_len,
                     (unsigned int)resp_len,
                     ret);
            return ret != 0 ? ret : -3;
        }

        offset += chunk_len;
    }

    return 0;
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
 * 5. 创建服务器 session，按 32KB 分片上传请求 WAV
 * 6. finish 后轮询 result_info，拿到回复 WAV 总长度
 * 7. 按 32KB 拉取 result_chunk，覆盖写回同一块 s_ai_wav_buf
 * 8. 校验回复 WAV 格式并播放 PCM
 * 9. 调用 app_business_audio_session_end() 释放音频会话锁
 * 10. 回到步骤 1，等待下一次录音完成
 *
 * ## 错误处理
 * - 任何步骤失败都通过 app_business_audio_session_end() 释放锁
 * - 不会因为单次失败导致锁泄漏或任务死锁
 *
 * ## 内存策略
 * - 请求 WAV 和响应 WAV 复用 s_ai_wav_buf（按 120 秒回复上限分配，PSRAM）
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

        /* 步骤 2-3：构造请求 WAV 文件（拷贝 PCM + 写文件头） */
        uint32_t pcm_bytes = samples_total * sizeof(int16_t);
        uint32_t wav_len = APP_BUSINESS_WAV_HEADER_LEN + pcm_bytes;
        if (pcm_bytes > 0u && wav_len <= APP_BUSINESS_AI_REQUEST_WAV_MAX_BYTES) {
            int16_t *pcm = (int16_t *)&s_ai_wav_buf[APP_BUSINESS_WAV_HEADER_LEN];
            memcpy(pcm, record_pcm, pcm_bytes);
            app_ai_voice_wav_write_header(s_ai_wav_buf, pcm_bytes);

            char session[64];
            ret = app_ai_voice_start_session(session, sizeof(session));
            if (ret == 0) {
                ret = app_ai_voice_upload_wav_chunks(session, wav_len);
            }
            if (ret == 0) {
                ret = app_ai_voice_finish_upload(session);
            }
            uint32_t reply_len = 0u;
            if (ret == 0) {
                ret = app_ai_voice_wait_result_info(session, &reply_len);
            }
            if (ret == 0) {
                if (reply_len > APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES) {
                    APP_LOGW(TAG,
                             "AI 回复 WAV 超限, len=%u, max=%u",
                             (unsigned int)reply_len,
                             (unsigned int)APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES);
                    ret = -10;
                } else {
                    ret = app_ai_voice_download_result_chunks(session, reply_len);
                }
            }

            /* 步骤 5-7：解析回复 WAV → 播放 AI 回答 */
            if (ret == 0 && reply_len > 0u) {
                const int16_t *resp_pcm = NULL;
                uint32_t resp_samples = 0u;
                /* HTTP body 经 AT 口返回时可能混入前缀字节，先定位 RIFF 再解析。 */
                int riff_pos = app_ai_voice_find_riff(s_ai_wav_buf, reply_len);
                if (riff_pos > 0) {
                    memmove(s_ai_wav_buf, &s_ai_wav_buf[riff_pos], reply_len - (uint32_t)riff_pos);
                    reply_len = reply_len - (uint32_t)riff_pos;
                }
                ret = app_ai_voice_wav_parse_pcm(s_ai_wav_buf, reply_len, &resp_pcm, &resp_samples);
                if (ret == 0 && resp_samples > 0u) {
                    /* 先标记播放开始（设置 s_playback_started 标志），再播放 PCM */
                    (void)service_audio_start_playback();
                    (void)service_audio_play(resp_pcm, resp_samples, 100u);
                } else {
                    APP_LOGW(TAG, "AI 响应 WAV 解析失败, ret=%d, len=%u", ret, (unsigned int)reply_len);
                }
            } else if (ret != 0) {
                APP_LOGW(TAG, "AI 分片问答失败, ret=%d", ret);
            }
        } else {
            APP_LOGW(TAG, "AI 录音 WAV 长度无效或超限, wav_len=%u", (unsigned int)wav_len);
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

    s_ai_wav_buf = (uint8_t *)heap_caps_malloc(APP_BUSINESS_AI_WAV_BUF_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ai_wav_buf == NULL) {
        APP_LOGE(TAG,
                 "AI WAV 缓存 PSRAM 分配失败, bytes=%u, psram_free=%u",
                 (unsigned int)APP_BUSINESS_AI_WAV_BUF_BYTES,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return -1;
    }

    APP_LOGI(TAG,
             "AI WAV 缓存已分配到 PSRAM, bytes=%u",
             (unsigned int)APP_BUSINESS_AI_WAV_BUF_BYTES);

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
