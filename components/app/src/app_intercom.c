/**
 * @file app_intercom.c
 * @brief UDP 实时对讲业务——PTT 发送、UDP 接收和心跳保持。
 *
 * ## 业务流程概述
 *
 * ### PTT 发送（biz_ptt 任务，优先级 6）
 * 1. 用户长按 UI 上的 PTT 按钮 → 回调 → app_intercom_ptt_start()
 * 2. 检查音频会话是否被 AI 占用 → 若空闲则置 s_ptt_active = 1
 * 3. notify_give(biz_ptt) → 唤醒 PTT 任务
 * 4. PTT 任务抢占音频会话锁 → 发 PTT_START 控制包
 * 5. 循环：读麦克风 320 samples(20ms) → 封装协议头 → UDP 发送
 * 6. 用户松手 → s_ptt_active = 0 → 循环退出 → 发 PTT_STOP
 * 7. 释放音频会话锁 → notify_take 阻塞等待下次
 *
 * ### UDP 接收（biz_udp_rx 任务，优先级 5）
 * 1. 轮询网络下行数据（80ms 超时）
 * 2. 按 WTK1 魔数定位完整包 → 解析 → 若为音频包且非本机 → 播放
 *
 * ### 心跳（biz_heartbeat 任务，优先级 4）
 * 1. 每 10 秒发一次 HEARTBEAT 包
 * 2. 检测到网络断开后自动重建 UDP 通道
 *
 * ## 自定义应用层协议（基于 WTK1 魔数）
 * ```
 * Byte 0-3:  "WTK1" 魔数
 * Byte 4:    包类型（1=REGISTER, 2=CHANNEL, 3=PTT_START, 4=AUDIO, 5=PTT_STOP, 6=HEARTBEAT）
 * Byte 5:    头长度（固定 34）
 * Byte 6-7:  频道号（uint16 LE）
 * Byte 8-11: 序列号（uint32 LE）
 * Byte 12-15: 时间戳（uint32 LE, ms）
 * Byte 16-31: 设备名（16 字节，不足补 0）
 * Byte 32-33: payload 长度（uint16 LE）
 * Byte 34+:  payload（仅 AUDIO 包有，PCM 16bit 单声道）
 * ```
 */
#include "app_intercom.h"

#include "app_business.h"
#include "app_config.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "app_intercom";

/** @brief 对讲应用层协议魔数，用于在 UDP 下行字节流中定位包头。 */
#define APP_INTERCOM_PACKET_MAGIC       "WTK1"
/** @brief 协议头中设备名字段固定长度，短设备名使用 0 填充。 */
#define APP_INTERCOM_DEVICE_FIELD_LEN   16u
/** @brief WTK1 协议固定包头长度。 */
#define APP_INTERCOM_PACKET_HEADER_LEN  34u
/** @brief 单个 AUDIO 包最大 payload，等于一帧 20ms PCM 字节数。 */
#define APP_INTERCOM_PACKET_MAX_PAYLOAD APP_BUSINESS_FRAME_BYTES
/** @brief 单个 WTK1 包最大总长度，包含固定头和最大音频 payload。 */
#define APP_INTERCOM_PACKET_MAX_BYTES   (APP_INTERCOM_PACKET_HEADER_LEN + APP_INTERCOM_PACKET_MAX_PAYLOAD)

/** @brief PTT 发送任务栈大小，需容纳协议包缓冲和 service_audio_read 调用栈。 */
#define APP_INTERCOM_PTT_TASK_STACK     6144u
/** @brief UDP 接收解析任务栈大小，播放启停会调用 codec/I2C，预留更深调用栈。 */
#define APP_INTERCOM_RX_TASK_STACK      6144u
/** @brief 心跳和 UDP 重连任务栈大小。 */
#define APP_INTERCOM_HEARTBEAT_STACK    6144u
/** @brief PTT 开始采集前等待 UDP 就绪的最长时间。 */
#define APP_INTERCOM_PTT_WAIT_UDP_MS    15000u
/** @brief PTT 等待 UDP 就绪时的轮询间隔。 */
#define APP_INTERCOM_PTT_WAIT_STEP_MS   100u
/** @brief UDP 接收播放空闲关闭时间，避免短抖动导致功放反复开关。 */
#define APP_INTERCOM_RX_PLAYBACK_IDLE_MS 240u
/** @brief UDP 接收空闲轮询超时。 */
#define APP_INTERCOM_RX_READ_IDLE_TIMEOUT_MS 20u
/** @brief UDP 接收播放中轮询超时，避免网络读取阻塞播放节奏。 */
#define APP_INTERCOM_RX_READ_ACTIVE_TIMEOUT_MS 4u
/** @brief UDP 对讲固定音频帧时长。 */
#define APP_INTERCOM_AUDIO_FRAME_MS      20u
/** @brief jitter buffer 容量，16 帧约 320ms。 */
#define APP_INTERCOM_JITTER_FRAME_COUNT  16u
/** @brief 起播缓存帧数，5 帧约 100ms，用于吸收公网抖动。 */
#define APP_INTERCOM_JITTER_START_FRAMES 5u
/** @brief jitter buffer 低水位，低于此值时减慢播放一拍等待网络追上。 */
#define APP_INTERCOM_JITTER_LOW_WATER    2u
/** @brief jitter buffer 高水位，超过此值时略微追帧降低延迟。 */
#define APP_INTERCOM_JITTER_HIGH_WATER   12u
/** @brief 连续缺帧达到该值且已有后续帧时，跳过缺口继续播放。 */
#define APP_INTERCOM_JITTER_RESYNC_MISSING 2u
/** @brief 连续缺帧补偿上限，超过后认为本次语音流中断。 */
#define APP_INTERCOM_JITTER_MAX_MISSING  12u
/** @brief 起播前等待后续帧的最长时间，超过后丢弃残留短流。 */
#define APP_INTERCOM_JITTER_PRIME_TIMEOUT_MS 300u

/** @brief 自定义应用层协议包类型枚举。 */
typedef enum {
    APP_INTERCOM_PKT_REGISTER = 1,  /**< 设备注册（上报设备名到服务器） */
    APP_INTERCOM_PKT_CHANNEL = 2,   /**< 频道切换 */
    APP_INTERCOM_PKT_PTT_START = 3, /**< PTT 开始（对讲键按下） */
    APP_INTERCOM_PKT_AUDIO = 4,     /**< 音频数据帧（20ms PCM） */
    APP_INTERCOM_PKT_PTT_STOP = 5,  /**< PTT 结束（对讲键松开） */
    APP_INTERCOM_PKT_HEARTBEAT = 6, /**< 心跳保活（10s 间隔） */
} app_intercom_packet_type_t;

/**
 * @brief 解析后的数据包视图——零拷贝设计。
 *
 * 解析时不复制 payload，直接指向原始 buffer 中的偏移位置，
 * 减少 20ms 音频帧的处理开销。
 */
typedef struct {
    uint8_t type;           /**< 包类型（见 app_intercom_packet_type_t） */
    uint16_t channel;       /**< 目标频道号 */
    uint32_t seq;           /**< 发送序列号（每包递增） */
    char device[APP_INTERCOM_DEVICE_FIELD_LEN + 1u]; /**< 发送端设备名。 */
    const uint8_t *payload; /**< payload 指针（指向原始 buffer 内部） */
    uint16_t payload_len;   /**< payload 长度（字节） */
    const uint8_t *packet;  /**< 完整包起始指针 */
    uint16_t packet_len;    /**< 完整包长度（头 + payload） */
} app_intercom_packet_view_t;

typedef struct {
    uint8_t valid;                                  /**< 槽位是否有可播放帧。 */
    uint32_t seq;                                  /**< 对应协议序列号。 */
    uint16_t samples;                              /**< PCM 样本数。 */
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];       /**< 固定 20ms PCM 帧。 */
} app_intercom_jitter_frame_t;

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief PTT 发送任务句柄，优先级 6（高于 biz_ai），
 *  确保 PTT 实时音频采集不被 AI 任务抢占。 */
static osal_task_t s_ptt_task = NULL;

/** @brief 心跳/重连任务句柄，网络切换后用于立即唤醒重连。 */
static osal_task_t s_heartbeat_task = NULL;

/** @brief 对讲模块是否已启动。 */
static volatile int s_started = 0;
/** @brief UDP 接收侧是否已打开本地播放输出。 */
static int s_rx_playback_active = 0;
/** @brief UDP 接收侧最近一次成功播放音频帧的时间。 */
static uint32_t s_rx_last_audio_ms = 0u;
/** @brief UDP 接收侧播放统计日志节流时间。 */
static uint32_t s_rx_play_log_ms = 0u;
/** @brief UDP 接收解析缓冲，放在静态区避免挤占 biz_udp_rx 任务栈。 */
static uint8_t s_rx_buf[APP_INTERCOM_PACKET_MAX_BYTES * 2u];
/** @brief UDP 接收播放 PCM 缓冲，单任务独占使用。 */
static int16_t s_rx_pcm[APP_BUSINESS_FRAME_SAMPLES];
/** @brief UDP 接收上一帧 PCM，用于缺包补偿。 */
static int16_t s_rx_last_pcm[APP_BUSINESS_FRAME_SAMPLES];
/** @brief UDP 接收 jitter buffer。 */
static app_intercom_jitter_frame_t s_rx_jitter[APP_INTERCOM_JITTER_FRAME_COUNT];
/** @brief jitter buffer 当前语音流来源设备名。 */
static char s_rx_jitter_device[APP_INTERCOM_DEVICE_FIELD_LEN + 1u];
/** @brief jitter buffer 下一帧期望播放的序列号。 */
static uint32_t s_rx_jitter_expected_seq = 0u;
/** @brief 下一次播放调度时间。 */
static uint32_t s_rx_jitter_next_play_ms = 0u;
/** @brief 最近一次收到当前 jitter 流音频包的时间。 */
static uint32_t s_rx_jitter_last_enqueue_ms = 0u;
/** @brief jitter buffer 是否已绑定当前语音流。 */
static uint8_t s_rx_jitter_ready = 0u;
/** @brief jitter buffer 是否已经起播。 */
static uint8_t s_rx_jitter_playing = 0u;
/** @brief 连续缺帧计数。 */
static uint8_t s_rx_jitter_missing = 0u;

/** @brief UDP 通道是否已连接就绪。
 *  由心跳任务在网络恢复后设置，PTT 发送前不检查此标志——即使 UDP 未就绪也尝试发送，
 *  底层 driver 会返回错误但不阻塞。 */
static volatile int s_udp_ready = 0;

/** @brief 当前是否处于 PTT 按下状态。
 *  PTT 任务在 while(s_ptt_active) 循环中采集+发送，此标志为 0 时退出循环。 */
static volatile int s_ptt_active = 0;

/** @brief 当前对讲频道号（1-32），默认频道 1。
 *  由 UI 频道切换或 PTT 按下时携带的频道号更新。 */
static int32_t s_current_channel = APP_BUSINESS_DEFAULT_CHANNEL;

/** @brief 全局 UDP 包序列号，每发一个包自增 1。
 *  用于服务器端去重和排序。 */
static uint32_t s_udp_seq = 0u;

/* ==========================================================================
 * 协议编解码
 * ========================================================================== */

/** @brief 写入协议 little-endian uint16 字段。 */
static void app_intercom_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

/** @brief 读取协议 little-endian uint16 字段。 */
static uint16_t app_intercom_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/** @brief 写入协议 little-endian uint32 字段。 */
static void app_intercom_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

/** @brief 读取协议 little-endian uint32 字段。 */
static uint32_t app_intercom_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/**
 * @brief 构建一个完整的 WTK1 协议包。
 *
 * 固定 34 字节头：
 * - 魔数 "WTK1"（4B）
 * - 类型（1B）
 * - 头长度（1B）= 34
 * - 频道号（2B LE）
 * - 序列号（4B LE，自动递增）
 * - 时间戳（4B LE，osal_get_tick_ms）
 * - 设备名（16B，短名补 0）
 * - payload 长度（2B LE）
 *
 * @param[out] out         输出缓冲区，至少 34 + payload_len 字节。
 * @param[in]  type        包类型。
 * @param[in]  payload     payload 数据（可为 NULL）。
 * @param[in]  payload_len payload 长度（可为 0）。
 * @return 实际包长度（头+payload）；失败返回 0。
 */
static uint16_t app_intercom_build_packet(uint8_t *out,
                                          uint8_t type,
                                          const uint8_t *payload,
                                          uint16_t payload_len)
{
    if (out == NULL || payload_len > APP_INTERCOM_PACKET_MAX_PAYLOAD) {
        return 0u;
    }

    memcpy(&out[0], APP_INTERCOM_PACKET_MAGIC, 4u);
    out[4] = type;
    out[5] = APP_INTERCOM_PACKET_HEADER_LEN;
    app_intercom_write_u16(&out[6], (uint16_t)s_current_channel);
    app_intercom_write_u32(&out[8], s_udp_seq++);
    app_intercom_write_u32(&out[12], osal_get_tick_ms());
    /* 设备名固定 16 字节，短名后面补 0，便于服务器原样转发和客户端比较。 */
    memset(&out[16], 0, APP_INTERCOM_DEVICE_FIELD_LEN);
    strncpy((char *)&out[16], APP_BUSINESS_DEVICE_NAME, APP_INTERCOM_DEVICE_FIELD_LEN - 1u);
    app_intercom_write_u16(&out[32], payload_len);

    if (payload != NULL && payload_len > 0u) {
        memcpy(&out[APP_INTERCOM_PACKET_HEADER_LEN], payload, payload_len);
    }

    return (uint16_t)(APP_INTERCOM_PACKET_HEADER_LEN + payload_len);
}

/**
 * @brief 解析原始字节流为数据包视图（零拷贝）。
 *
 * 只做校验和建立 view 结构体，不复制 payload 数据。
 *
 * @param[in]  packet 原始数据包缓冲区。
 * @param[in]  len    缓冲区长度。
 * @param[out] view   解析结果输出。
 * @return 0=成功；-1=参数无效；-2=魔数不匹配；-3=长度校验失败。
 */
static int app_intercom_parse_packet(const uint8_t *packet,
                                     uint16_t len,
                                     app_intercom_packet_view_t *view)
{
    if (packet == NULL || view == NULL || len < APP_INTERCOM_PACKET_HEADER_LEN) {
        return -1;
    }
    if (memcmp(packet, APP_INTERCOM_PACKET_MAGIC, 4u) != 0) {
        return -2;
    }

    uint8_t header_len = packet[5];
    uint16_t payload_len = app_intercom_read_u16(&packet[32]);
    /* 解析阶段只建立 view，不复制 payload，减少 20ms 音频包处理开销。 */
    if (header_len != APP_INTERCOM_PACKET_HEADER_LEN ||
        len < (uint16_t)(header_len + payload_len)) {
        return -3;
    }

    view->type = packet[4];
    view->channel = app_intercom_read_u16(&packet[6]);
    view->seq = app_intercom_read_u32(&packet[8]);
    memcpy(view->device, &packet[16], APP_INTERCOM_DEVICE_FIELD_LEN);
    view->device[APP_INTERCOM_DEVICE_FIELD_LEN] = '\0';
    view->payload = &packet[header_len];
    view->payload_len = payload_len;
    view->packet = packet;
    view->packet_len = (uint16_t)(header_len + payload_len);
    return 0;
}

/**
 * @brief 判断数据包是否来自本机。
 *
 * 服务器会原样转发所有音频包（包括本机的），
 * 客户端通过比较设备名字段来过滤掉自己的包，避免回声。
 *
 * @return 1=是本机发出的包；0=非本机。
 */
static int app_intercom_packet_is_own(const uint8_t *packet)
{
    char name[APP_INTERCOM_DEVICE_FIELD_LEN + 1u];

    if (packet == NULL) {
        return 0;
    }

    memcpy(name, &packet[16], APP_INTERCOM_DEVICE_FIELD_LEN);
    name[APP_INTERCOM_DEVICE_FIELD_LEN] = '\0';
    return strncmp(name, APP_BUSINESS_DEVICE_NAME, APP_INTERCOM_DEVICE_FIELD_LEN) == 0;
}

/**
 * @brief 发送无 payload 的控制包（注册/频道/PTT_START/PTT_STOP/心跳）。
 *
 * @param type 包类型。
 * @return 成功返回 0；UDP 未就绪返回 -1。
 */
static int app_intercom_send_control(uint8_t type)
{
    if (!s_udp_ready) {
        return -1;
    }

    /* 控制包没有 payload，用于服务器维护设备在线状态和频道状态。 */
    uint8_t packet[APP_INTERCOM_PACKET_HEADER_LEN];
    uint16_t len = app_intercom_build_packet(packet, type, NULL, 0u);
    return service_network_udp_send(packet, len);
}

static void app_intercom_reconnect_udp_if_ready(void)
{
    if (s_udp_ready) {
        return;
    }
    int ready = service_network_is_ready();
    if (ready != 1) {
        APP_LOGW(TAG, "UDP 对讲通道暂不可重连, network_ready=%d", ready);
        return;
    }

    int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
    if (ret == 0) {
        s_udp_ready = 1;
        APP_LOGI(TAG, "UDP 对讲通道已连接");
        (void)app_intercom_send_control(APP_INTERCOM_PKT_REGISTER);
        (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);
    } else {
        APP_LOGW(TAG, "UDP 对讲通道重连失败, ret=%d", ret);
    }
}

static int app_intercom_wait_udp_ready(uint32_t timeout_ms)
{
    uint32_t start = osal_get_tick_ms();

    while (s_ptt_active && !s_udp_ready && (osal_get_tick_ms() - start) < timeout_ms) {
        app_intercom_reconnect_udp_if_ready();
        if (s_udp_ready) {
            return 0;
        }
        osal_delay_ms(APP_INTERCOM_PTT_WAIT_STEP_MS);
    }

    return s_udp_ready ? 0 : -1;
}

static void app_intercom_rx_stop_playback(void)
{
    if (!s_rx_playback_active) {
        return;
    }

    (void)service_audio_stop_playback();
    s_rx_playback_active = 0;
}

static int app_intercom_rx_start_playback(void)
{
    if (s_rx_playback_active) {
        return 0;
    }

    int ret = service_audio_start_playback();
    if (ret != 0) {
        return ret;
    }

    s_rx_playback_active = 1;
    return 0;
}

static void app_intercom_rx_stop_if_idle(void)
{
    if (!s_rx_playback_active) {
        return;
    }

    uint32_t now = osal_get_tick_ms();
    if ((uint32_t)(now - s_rx_last_audio_ms) >= APP_INTERCOM_RX_PLAYBACK_IDLE_MS) {
        app_intercom_rx_stop_playback();
    }
}

static int app_intercom_seq_before(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b)) < 0;
}

static void app_intercom_jitter_clear(void)
{
    memset(s_rx_jitter, 0, sizeof(s_rx_jitter));
    memset(s_rx_last_pcm, 0, sizeof(s_rx_last_pcm));
    s_rx_jitter_device[0] = '\0';
    s_rx_jitter_expected_seq = 0u;
    s_rx_jitter_next_play_ms = 0u;
    s_rx_jitter_last_enqueue_ms = 0u;
    s_rx_jitter_ready = 0u;
    s_rx_jitter_playing = 0u;
    s_rx_jitter_missing = 0u;
}

static uint8_t app_intercom_jitter_count_ready(void)
{
    uint8_t count = 0u;

    for (uint32_t i = 0u; i < APP_INTERCOM_JITTER_FRAME_COUNT; i++) {
        if (s_rx_jitter[i].valid &&
            !app_intercom_seq_before(s_rx_jitter[i].seq, s_rx_jitter_expected_seq)) {
            count++;
        }
    }

    return count;
}

static app_intercom_jitter_frame_t *app_intercom_jitter_find(uint32_t seq)
{
    for (uint32_t i = 0u; i < APP_INTERCOM_JITTER_FRAME_COUNT; i++) {
        if (s_rx_jitter[i].valid && s_rx_jitter[i].seq == seq) {
            return &s_rx_jitter[i];
        }
    }

    return NULL;
}

static int app_intercom_jitter_find_next_seq(uint32_t from_seq, uint32_t *next_seq)
{
    uint32_t best = 0u;
    int found = 0;

    if (next_seq == NULL) {
        return 0;
    }

    for (uint32_t i = 0u; i < APP_INTERCOM_JITTER_FRAME_COUNT; i++) {
        if (s_rx_jitter[i].valid &&
            !app_intercom_seq_before(s_rx_jitter[i].seq, from_seq) &&
            (found == 0 || app_intercom_seq_before(s_rx_jitter[i].seq, best))) {
            best = s_rx_jitter[i].seq;
            found = 1;
        }
    }

    if (found != 0) {
        *next_seq = best;
    }
    return found;
}

static void app_intercom_jitter_drop_before(uint32_t seq)
{
    for (uint32_t i = 0u; i < APP_INTERCOM_JITTER_FRAME_COUNT; i++) {
        if (s_rx_jitter[i].valid && app_intercom_seq_before(s_rx_jitter[i].seq, seq)) {
            s_rx_jitter[i].valid = 0u;
        }
    }
}

static void app_intercom_jitter_reset_for_source(const char *device, uint32_t seq)
{
    app_intercom_jitter_clear();
    if (device != NULL) {
        strncpy(s_rx_jitter_device, device, sizeof(s_rx_jitter_device) - 1u);
        s_rx_jitter_device[sizeof(s_rx_jitter_device) - 1u] = '\0';
    }
    s_rx_jitter_expected_seq = seq;
    s_rx_jitter_ready = 1u;
}

static void app_intercom_jitter_enqueue(const app_intercom_packet_view_t *view)
{
    if (view == NULL || view->payload == NULL || view->payload_len < sizeof(int16_t)) {
        return;
    }

    if (s_rx_jitter_ready == 0u ||
        strncmp(s_rx_jitter_device, view->device, APP_INTERCOM_DEVICE_FIELD_LEN) != 0) {
        app_intercom_jitter_reset_for_source(view->device, view->seq);
    }

    if (s_rx_jitter_playing && app_intercom_seq_before(view->seq, s_rx_jitter_expected_seq)) {
        return;
    }

    uint32_t ahead = view->seq - s_rx_jitter_expected_seq;
    if (ahead >= APP_INTERCOM_JITTER_FRAME_COUNT) {
        s_rx_jitter_expected_seq = view->seq - (APP_INTERCOM_JITTER_FRAME_COUNT - 1u);
        app_intercom_jitter_drop_before(s_rx_jitter_expected_seq);
    }

    uint32_t slot_index = view->seq % APP_INTERCOM_JITTER_FRAME_COUNT;
    app_intercom_jitter_frame_t *frame = &s_rx_jitter[slot_index];
    if (frame->valid && frame->seq == view->seq) {
        return;
    }

    uint16_t samples = (uint16_t)(view->payload_len / sizeof(int16_t));
    if (samples > APP_BUSINESS_FRAME_SAMPLES) {
        samples = APP_BUSINESS_FRAME_SAMPLES;
    }

    memset(frame->pcm, 0, sizeof(frame->pcm));
    memcpy(frame->pcm, view->payload, (size_t)samples * sizeof(int16_t));
    frame->samples = samples;
    frame->seq = view->seq;
    frame->valid = 1u;
    s_rx_jitter_last_enqueue_ms = osal_get_tick_ms();
}

static void app_intercom_jitter_make_plc_frame(int16_t *out)
{
    int scale = 0;

    if (s_rx_jitter_missing == 1u) {
        scale = 3;
    } else if (s_rx_jitter_missing == 2u) {
        scale = 2;
    } else if (s_rx_jitter_missing == 3u) {
        scale = 1;
    }

    for (uint32_t i = 0u; i < APP_BUSINESS_FRAME_SAMPLES; i++) {
        out[i] = (int16_t)(((int32_t)s_rx_last_pcm[i] * scale) / 4);
    }
}

static uint32_t app_intercom_rx_read_timeout_ms(void)
{
    return s_rx_jitter_playing != 0u ?
           APP_INTERCOM_RX_READ_ACTIVE_TIMEOUT_MS :
           APP_INTERCOM_RX_READ_IDLE_TIMEOUT_MS;
}

static void app_intercom_jitter_play_tick(void)
{
    uint32_t now = osal_get_tick_ms();

    if (s_rx_jitter_ready == 0u) {
        return;
    }

    if (s_rx_jitter_playing == 0u) {
        if (app_intercom_jitter_count_ready() < APP_INTERCOM_JITTER_START_FRAMES) {
            if (s_rx_jitter_last_enqueue_ms != 0u &&
                (uint32_t)(now - s_rx_jitter_last_enqueue_ms) >= APP_INTERCOM_JITTER_PRIME_TIMEOUT_MS) {
                app_intercom_jitter_clear();
            }
            return;
        }
        if (app_intercom_rx_start_playback() != 0) {
            app_intercom_jitter_clear();
            return;
        }
        s_rx_jitter_playing = 1u;
        s_rx_jitter_next_play_ms = now;
        s_rx_jitter_missing = 0u;
        APP_LOGI(TAG, "UDP 音频 jitter 起播, device=%s, seq=%u",
                 s_rx_jitter_device,
                 (unsigned int)s_rx_jitter_expected_seq);
    }

    if (app_intercom_seq_before(now, s_rx_jitter_next_play_ms)) {
        return;
    }
    if ((uint32_t)(now - s_rx_jitter_next_play_ms) > 100u) {
        s_rx_jitter_next_play_ms = now;
    }

    app_intercom_jitter_frame_t *frame = app_intercom_jitter_find(s_rx_jitter_expected_seq);
    uint16_t samples = APP_BUSINESS_FRAME_SAMPLES;
    if (frame != NULL) {
        memcpy(s_rx_pcm, frame->pcm, sizeof(s_rx_pcm));
        memcpy(s_rx_last_pcm, frame->pcm, sizeof(s_rx_last_pcm));
        samples = frame->samples;
        frame->valid = 0u;
        s_rx_jitter_missing = 0u;
    } else {
        s_rx_jitter_missing++;
        if (s_rx_jitter_missing >= APP_INTERCOM_JITTER_RESYNC_MISSING) {
            uint32_t next_seq = 0u;
            if (app_intercom_jitter_find_next_seq(s_rx_jitter_expected_seq, &next_seq) != 0 &&
                next_seq != s_rx_jitter_expected_seq) {
                uint32_t skipped = next_seq - s_rx_jitter_expected_seq;
                APP_LOGW(TAG, "UDP 音频跳过缺口, device=%s, from=%u, to=%u, skipped=%u, buffered=%u",
                         s_rx_jitter_device,
                         (unsigned int)s_rx_jitter_expected_seq,
                         (unsigned int)next_seq,
                         (unsigned int)skipped,
                         (unsigned int)app_intercom_jitter_count_ready());
                s_rx_jitter_expected_seq = next_seq;
                s_rx_jitter_missing = 0u;
                frame = app_intercom_jitter_find(s_rx_jitter_expected_seq);
                if (frame != NULL) {
                    memcpy(s_rx_pcm, frame->pcm, sizeof(s_rx_pcm));
                    memcpy(s_rx_last_pcm, frame->pcm, sizeof(s_rx_last_pcm));
                    samples = frame->samples;
                    frame->valid = 0u;
                } else {
                    app_intercom_jitter_make_plc_frame(s_rx_pcm);
                }
            } else {
                app_intercom_jitter_make_plc_frame(s_rx_pcm);
            }
        } else {
            app_intercom_jitter_make_plc_frame(s_rx_pcm);
        }
        if (s_rx_jitter_missing > APP_INTERCOM_JITTER_MAX_MISSING) {
            APP_LOGW(TAG, "UDP 音频流中断, device=%s, seq=%u",
                     s_rx_jitter_device,
                     (unsigned int)s_rx_jitter_expected_seq);
            app_intercom_rx_stop_playback();
            app_intercom_jitter_clear();
            return;
        }
    }

    int played = service_audio_play(s_rx_pcm, samples, 30u);
    if (played < 0) {
        APP_LOGW(TAG, "UDP 音频播放失败, seq=%u, ret=%d",
                 (unsigned int)s_rx_jitter_expected_seq,
                 played);
        app_intercom_rx_stop_playback();
        app_intercom_jitter_clear();
        return;
    }

    if (played > 0) {
        s_rx_last_audio_ms = now;
        if (s_rx_last_audio_ms - s_rx_play_log_ms >= 1000u) {
            APP_LOGI(TAG, "UDP 音频播放中, device=%s, seq=%u, samples=%d, buffered=%u, missing=%u",
                     s_rx_jitter_device,
                     (unsigned int)s_rx_jitter_expected_seq,
                     played,
                     (unsigned int)app_intercom_jitter_count_ready(),
                     (unsigned int)s_rx_jitter_missing);
            s_rx_play_log_ms = s_rx_last_audio_ms;
        }
    }

    s_rx_jitter_expected_seq++;
    uint8_t buffered_after = app_intercom_jitter_count_ready();
    if (buffered_after >= APP_INTERCOM_JITTER_HIGH_WATER) {
        s_rx_jitter_next_play_ms += (APP_INTERCOM_AUDIO_FRAME_MS / 2u);
    } else if (buffered_after > 0u && buffered_after <= APP_INTERCOM_JITTER_LOW_WATER) {
        s_rx_jitter_next_play_ms += (APP_INTERCOM_AUDIO_FRAME_MS + (APP_INTERCOM_AUDIO_FRAME_MS / 2u));
    } else {
        s_rx_jitter_next_play_ms += APP_INTERCOM_AUDIO_FRAME_MS;
    }
}

/* ==========================================================================
 * 心跳任务 —— 保持服务器在线状态 + 断线自动重连
 * ========================================================================== */

/**
 * @brief 心跳任务入口。
 *
 * ## 功能
 * 1. 启动时发送 REGISTER + CHANNEL 包（注册设备到服务器）
 * 2. 每 10 秒发送 HEARTBEAT 保活
 * 3. 检测网络后端 ready 后自动重连 UDP
 * 4. 重连后重新发送 REGISTER + CHANNEL，恢复在线状态
 *
 * ## 重连机制
 * - 网络切换或 PTT 按下时通过 notify 立即唤醒心跳任务
 * - 心跳任务每 10s 兜底检查 service_network_is_ready() 和 s_udp_ready
 * - 若网络已恢复但 UDP 通道未建立 → 调用 service_network_udp_connect() 重建
 *
 * @param arg 未使用。
 */
static void app_intercom_heartbeat_task(void *arg)
{
    (void)arg;
    (void)app_intercom_send_control(APP_INTERCOM_PKT_REGISTER);
    (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);

    while (1) {
        /* 网络恢复后在后台重建 UDP 通道，并重新上报设备和频道。 */
        app_intercom_reconnect_udp_if_ready();
        (void)app_intercom_send_control(APP_INTERCOM_PKT_HEARTBEAT);
        (void)osal_task_notify_take(10000u);
    }
}

/* ==========================================================================
 * UDP 接收任务
 * ========================================================================== */

/**
 * @brief 处理接收到的单帧完整 UDP 包。
 *
 * ## 过滤规则
 * 1. 包频道 != 当前频道 → 忽略（不同频道的人说话听不到）
 * 2. 包来自本机 → 忽略（服务器会原样转发，过滤掉自己的回声）
 * 3. 包类型 != AUDIO → 忽略（控制包不需要本地处理）
 *
 * ## 播放
 * 如果是有效的音频包，将 payload（PCM 16bit）放入 jitter buffer。
 *
 * @param packet 完整包数据。
 * @param len    包长度。
 */
static void app_intercom_handle_udp_packet(const uint8_t *packet, uint16_t len)
{
    app_intercom_packet_view_t view;
    if (app_intercom_parse_packet(packet, len, &view) != 0) {
        return;
    }

    /* 服务器会原样转发音频包，本机自己的包和非当前频道包都直接忽略。 */
    if (view.channel != (uint16_t)s_current_channel ||
        app_intercom_packet_is_own(packet)) {
        return;
    }

    if (view.type == APP_INTERCOM_PKT_AUDIO && view.payload_len > 0u) {
        app_intercom_jitter_enqueue(&view);
    }
}

/**
 * @brief UDP 接收任务入口。
 *
 * ## 粘包/半包处理
 * UDP 下行数据可能粘包或半包：
 * - 粘包：一次 read 可能返回多个完整包
 * - 半包：一个完整包可能跨两次 read
 *
 * ## 处理策略
 * 1. 维护一个内部接收环形缓冲区 rx[1348]（= 最大包长 × 2）
 * 2. 每次读取追加到 used 之后
 * 3. 从 pos 开始扫描 WTK1 魔数
 * 4. 找到魔数 → 解析头长度和 payload 长度 → 判断是否完整包
 * 5. 完整包 → 调用 handle_udp_packet() → pos 后移
 * 6. 半包 → break 等待下次读取
 * 7. 非魔数字节 → pos++ 继续扫描
 * 8. 消费完后 memmove 剩余数据到缓冲区头部
 *
 * @param arg 未使用。
 */
static void app_intercom_udp_rx_task(void *arg)
{
    (void)arg;
    uint16_t used = 0u;

    while (1) {
        app_intercom_jitter_play_tick();

        int ret = service_network_read_downlink(&s_rx_buf[used],
                                                (uint16_t)(sizeof(s_rx_buf) - used),
                                                app_intercom_rx_read_timeout_ms());
        if (ret > 0) {
            used = (uint16_t)(used + ret);
            uint16_t pos = 0u;
            /* 下行数据可能跨多次读取，保留半包并只消费完整 WTK1 包。 */
            while ((pos + APP_INTERCOM_PACKET_HEADER_LEN) <= used) {
                if (memcmp(&s_rx_buf[pos], APP_INTERCOM_PACKET_MAGIC, 4u) != 0) {
                    pos++;
                    continue;
                }

                uint8_t header_len = s_rx_buf[pos + 5u];
                uint16_t payload_len = (uint16_t)s_rx_buf[pos + 32u] | ((uint16_t)s_rx_buf[pos + 33u] << 8);
                uint16_t packet_len = (uint16_t)(header_len + payload_len);
                if (header_len != APP_INTERCOM_PACKET_HEADER_LEN || packet_len > APP_INTERCOM_PACKET_MAX_BYTES) {
                    pos++;
                    continue;
                }
                if ((pos + packet_len) > used) {
                    break;
                }

                app_intercom_handle_udp_packet(&s_rx_buf[pos], packet_len);
                pos = (uint16_t)(pos + packet_len);
            }

            if (pos > 0u) {
                memmove(s_rx_buf, &s_rx_buf[pos], used - pos);
                used = (uint16_t)(used - pos);
            }
            if (used >= sizeof(s_rx_buf)) {
                used = 0u;
            }
        } else if (ret < 0 || s_rx_jitter_playing == 0u) {
            osal_delay_ms(10u);
        }
        app_intercom_jitter_play_tick();
        app_intercom_rx_stop_if_idle();
    }
}

/* ==========================================================================
 * PTT 发送任务 —— 按键 → 采集 → 封装 → 发送
 * ========================================================================== */

/**
 * @brief PTT 发送任务入口。
 *
 * ## 执行流程（每次被 notify 唤醒一次 = 一次完整的"按下到松开"）
 *
 * 1. 阻塞等待 notify（来自 app_intercom_ptt_start()）
 * 2. 检查 s_ptt_active 是否为 1 + 抢占音频会话锁（try_begin）
 * 3. 发送 PTT_START 控制包（通知服务器和同频道其他人）
 * 4. 循环（while (s_ptt_active)）：
 *    a. service_audio_read(pcm, 320, 30ms) —— 读 20ms PCM 帧
 *    b. app_intercom_build_packet(packet, AUDIO, pcm, 640)
 *    c. service_network_udp_send(packet, len) —— 发给服务器
 *    d. 服务器收到后原样转发给同频道所有其他客户端
 * 5. 用户松手 → s_ptt_active = 0 → 退出循环
 * 6. 发送 PTT_STOP 控制包
 * 7. 打印发送统计（成功/失败帧数）
 * 8. 释放音频会话锁 → notify_take 阻塞等待下次
 *
 * ## 性能约束
 * - 每帧 20ms (320 samples × 16bit = 640 字节)
 * - service_audio_read 超时 30ms，确保最坏情况下也不丢帧
 * - PTT 任务优先级 6（高于 AI 的 5），保证实时性
 *
 * @param arg 未使用。
 */
static void app_intercom_ptt_task(void *arg)
{
    (void)arg;
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];       /* 20ms PCM 帧缓冲 */
    uint8_t packet[APP_INTERCOM_PACKET_MAX_BYTES];  /* 协议包缓冲 */

    while (1) {
        (void)osal_task_notify_take(OSAL_WAIT_FOREVER);
        if (!s_ptt_active) {
            continue;
        }

        if (app_intercom_wait_udp_ready(APP_INTERCOM_PTT_WAIT_UDP_MS) != 0) {
            APP_LOGW(TAG, "PTT 放弃发送: UDP 对讲通道未就绪");
            continue;
        }

        if (!s_ptt_active || app_business_audio_session_try_begin() != 0) {
            continue;
        }

        /* PTT 期间按固定 20ms PCM 帧发送，第一版不做编解码和重传。 */
        (void)app_intercom_send_control(APP_INTERCOM_PKT_PTT_START);
        uint32_t read_ok = 0u;
        uint32_t read_fail = 0u;
        uint32_t send_ok = 0u;
        uint32_t send_fail = 0u;
        int last_read_ret = 0;
        int last_send_ret = 0;
        while (s_ptt_active) {
            int samples = service_audio_read(pcm, APP_BUSINESS_FRAME_SAMPLES, 30u);
            if (samples > 0) {
                read_ok++;
                uint16_t payload_len = (uint16_t)(samples * sizeof(int16_t));
                uint16_t packet_len = app_intercom_build_packet(packet,
                                                                APP_INTERCOM_PKT_AUDIO,
                                                                (const uint8_t *)pcm,
                                                                payload_len);
                if (packet_len > 0u) {
                    int send_ret = service_network_udp_send(packet, packet_len);
                    if (send_ret == 0) {
                        send_ok++;
                    } else {
                        send_fail++;
                        last_send_ret = send_ret;
                    }
                } else {
                    send_fail++;
                    last_send_ret = -100;
                }
            } else {
                read_fail++;
                last_read_ret = samples;
            }
        }
        (void)app_intercom_send_control(APP_INTERCOM_PKT_PTT_STOP);
        APP_LOGI(TAG,
                 "PTT 发送统计: read_ok=%u, read_fail=%u, send_ok=%u, send_fail=%u, "
                 "last_read=%d, last_send=%d",
                 (unsigned int)read_ok,
                 (unsigned int)read_fail,
                 (unsigned int)send_ok,
                 (unsigned int)send_fail,
                 last_read_ret,
                 last_send_ret);
        app_business_audio_session_end();
    }
}

/* ==========================================================================
 * 公开接口
 * ========================================================================== */

/**
 * @brief 启动对讲模块（开机时由 app_business_start 调用）。
 *
 * ## 创建的任务
 * - biz_ptt（优先级 6, 栈 6144）—— PTT 发送
 * - biz_udp_rx（优先级 5, 栈 4096）—— UDP 接收
 * - biz_heartbeat（优先级 4, 栈 6144）—— 心跳 + 重连
 *
 * 同时尝试首次 UDP 连接。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_intercom_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
    if (ret != 0) {
        APP_LOGW(TAG, "UDP 对讲通道初始化失败, ret=%d", ret);
    } else {
        s_udp_ready = 1;
    }

    ret = osal_task_create("biz_ptt",
                           app_intercom_ptt_task,
                           NULL,
                           APP_INTERCOM_PTT_TASK_STACK,
                           6u,
                           &s_ptt_task);
    if (ret != 0) {
        APP_LOGE(TAG, "PTT 任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_udp_rx",
                           app_intercom_udp_rx_task,
                           NULL,
                           APP_INTERCOM_RX_TASK_STACK,
                           5u,
                           NULL);
    if (ret != 0) {
        APP_LOGE(TAG, "UDP 接收任务启动失败, ret=%d", ret);
        return ret;
    }

    ret = osal_task_create("biz_heartbeat",
                           app_intercom_heartbeat_task,
                           NULL,
                           APP_INTERCOM_HEARTBEAT_STACK,
                           4u,
                           &s_heartbeat_task);
    if (ret != 0) {
        APP_LOGE(TAG, "对讲心跳任务启动失败, ret=%d", ret);
        return ret;
    }

    if (!s_udp_ready && s_heartbeat_task != NULL) {
        (void)osal_task_notify_give(s_heartbeat_task);
    }

    s_started = 1;
    return 0;
}

/**
 * @brief 切换对讲频道。
 *
 * 更新 s_current_channel → 立即发送 CHANNEL 控制包通知服务器。
 * 之后收到的 UDP 音频包只有匹配当前频道的才会被播放。
 *
 * @param channel 新频道号（1-32），非法值会被钳位到默认频道。
 */
void app_intercom_set_channel(int32_t channel)
{
    if (channel <= 0) {
        channel = APP_BUSINESS_DEFAULT_CHANNEL;
    }
    s_current_channel = channel;
    (void)app_intercom_send_control(APP_INTERCOM_PKT_CHANNEL);
}

/**
 * @brief PTT 按下——启动对讲发送。
 *
 * ## 流程
 * 1. 更新频道号（如果有传入）
 * 2. 检查音频会话是否被 AI 占用
 * 3. 设置 s_ptt_active = 1
 * 4. notify_give(s_ptt_task) → 唤醒 biz_ptt 任务开始采集+发送
 *
 * ## 与 AI 互斥
 * 如果 AI 正在录音（s_audio_session_busy == 1），则 is_busy() 返回 1，
 * 本函数直接 return，本次 PTT 操作被忽略。用户需要等 AI 处理完成后再按。
 *
 * @param channel 当前对讲频道号。
 */
void app_intercom_ptt_start(int32_t channel)
{
    s_current_channel = channel > 0 ? channel : s_current_channel;
    if (!s_udp_ready && s_heartbeat_task != NULL) {
        (void)osal_task_notify_give(s_heartbeat_task);
    }
    if (app_business_audio_session_is_busy()) {
        return;
    }

    s_ptt_active = 1;
    if (s_ptt_task != NULL) {
        (void)osal_task_notify_give(s_ptt_task);
    }
}

/**
 * @brief PTT 松开——停止对讲发送。
 *
 * 清零 s_ptt_active，PTT 任务在下次循环迭代时检测到此标志为 0，
 * 退出内层 while 循环，发送 PTT_STOP 并释放音频会话锁。
 */
void app_intercom_ptt_stop(void)
{
    s_ptt_active = 0;
}

void app_intercom_network_changed(void)
{
    s_udp_ready = 0;
    if (s_heartbeat_task != NULL) {
        (void)osal_task_notify_give(s_heartbeat_task);
    }
}
