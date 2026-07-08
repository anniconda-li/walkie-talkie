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
 * 1. 空闲时每 3 秒发一次 HEARTBEAT 包
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
 * Byte 34+:  payload（AUDIO 为 PCM 16bit 单声道，固定 20ms）
 * ```
 */
#include "app_intercom.h"

#include "app_business.h"
#include "app_config.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "service_audio.h"
#include "service_network.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "app_intercom";

/** @brief 对讲应用层协议魔数，用于在 UDP 下行字节流中定位包头。 */
#define APP_INTERCOM_PACKET_MAGIC       "WTK1"
/** @brief 协议头中设备名字段固定长度，短设备名使用 0 填充。 */
#define APP_INTERCOM_DEVICE_FIELD_LEN   16u
/** @brief WTK1 协议固定包头长度。 */
#define APP_INTERCOM_PACKET_HEADER_LEN  34u
/** @brief 单个 AUDIO 包聚合的 20ms PCM 帧数，1 帧即 20ms/包。 */
#define APP_INTERCOM_PACKET_FRAMES      1u
/** @brief 单个 AUDIO 包最大 PCM 样本数。 */
#define APP_INTERCOM_PACKET_SAMPLES     (APP_BUSINESS_FRAME_SAMPLES * APP_INTERCOM_PACKET_FRAMES)
/** @brief 单个 AUDIO 包 payload，当前为 20ms PCM 字节数。 */
#define APP_INTERCOM_AUDIO_PAYLOAD_BYTES (APP_INTERCOM_PACKET_SAMPLES * sizeof(int16_t))
/** @brief FEC 保护的 AUDIO 包数量，第一版每 4 包生成 1 个 XOR 校验包。 */
#define APP_INTERCOM_FEC_GROUP_SIZE      4u
/** @brief FEC payload 头：base_seq(4) + count(1) + payload_len(2) + reserved(1)。 */
#define APP_INTERCOM_FEC_HEADER_LEN      8u
/** @brief 单个 FEC payload 长度，包含 FEC 头和 XOR 后的 20ms PCM payload。 */
#define APP_INTERCOM_FEC_PAYLOAD_LEN     (APP_INTERCOM_FEC_HEADER_LEN + APP_INTERCOM_AUDIO_PAYLOAD_BYTES)
/** @brief 单个 WTK1 包最大 payload，需容纳 AUDIO 和 FEC。 */
#define APP_INTERCOM_PACKET_MAX_PAYLOAD  APP_INTERCOM_FEC_PAYLOAD_LEN
/** @brief 单个 WTK1 包最大总长度，包含固定头和最大业务 payload。 */
#define APP_INTERCOM_PACKET_MAX_BYTES   (APP_INTERCOM_PACKET_HEADER_LEN + APP_INTERCOM_PACKET_MAX_PAYLOAD)

/** @brief PTT 发送任务栈大小，需容纳协议包缓冲和 service_audio_read 调用栈。 */
#define APP_INTERCOM_PTT_TASK_STACK     8192u
/** @brief UDP 接收解析任务栈大小，播放和日志格式化共用该任务，预留更深调用栈。 */
#define APP_INTERCOM_RX_TASK_STACK      10240u
/** @brief 心跳和 UDP 重连任务栈大小。 */
#define APP_INTERCOM_HEARTBEAT_STACK    6144u
/** @brief PTT 开始采集前等待 UDP 就绪的最长时间。 */
#define APP_INTERCOM_PTT_WAIT_UDP_MS    15000u
/** @brief PTT 等待 UDP 就绪时的轮询间隔。 */
#define APP_INTERCOM_PTT_WAIT_STEP_MS   100u
/** @brief 空闲 UDP 心跳间隔，用于保持接收端 NAT/UDP 映射活跃。 */
#define APP_INTERCOM_HEARTBEAT_IDLE_MS  3000u
/** @brief 音频忙时不发心跳，只用该间隔继续检查状态。 */
#define APP_INTERCOM_HEARTBEAT_BUSY_MS  500u
/** @brief UDP 接收播放空闲关闭时间，延长以避免弱网短断流导致功放反复开关。 */
#define APP_INTERCOM_RX_PLAYBACK_IDLE_MS 500u
/** @brief UDP 接收空闲轮询超时。 */
#define APP_INTERCOM_RX_READ_IDLE_TIMEOUT_MS 20u
/** @brief UDP 接收播放中轮询超时，避免网络读取阻塞播放节奏。 */
#define APP_INTERCOM_RX_READ_ACTIVE_TIMEOUT_MS 4u
/** @brief UDP 对讲固定音频包时长。 */
#define APP_INTERCOM_AUDIO_FRAME_MS      (20u * APP_INTERCOM_PACKET_FRAMES)
/** @brief jitter buffer 容量，64 包约 1280ms，用延迟换弱网播放连续性。 */
#define APP_INTERCOM_JITTER_FRAME_COUNT  64u
/** @brief 正常网络下的起播缓存包数，20 包约 400ms。 */
#define APP_INTERCOM_JITTER_START_FRAMES 20u
/** @brief 抖动网络下的最大起播缓存包数，32 包约 640ms。 */
#define APP_INTERCOM_JITTER_MAX_START_FRAMES 32u
/** @brief 稳定播放这么多包后，逐步降低自适应起播水位。 */
#define APP_INTERCOM_JITTER_RECOVER_FRAMES 800u
/** @brief jitter buffer 低水位，低于此值时减慢播放一拍等待网络追上。 */
#define APP_INTERCOM_JITTER_LOW_WATER    2u
/** @brief jitter buffer 高水位，超过此值时略微追帧降低延迟。 */
#define APP_INTERCOM_JITTER_HIGH_WATER   48u
/** @brief 连续缺帧达到该值且已有后续帧时，跳过缺口继续播放。 */
#define APP_INTERCOM_JITTER_RESYNC_MISSING 2u
/** @brief 连续缺包补偿上限，超过后认为本次语音流中断。 */
#define APP_INTERCOM_JITTER_MAX_MISSING  40u
/** @brief 起播前等待后续帧的最长时间，超过后丢弃残留短流。 */
#define APP_INTERCOM_JITTER_PRIME_TIMEOUT_MS 1200u
/** @brief 缺包跳帧日志节流，避免弱网下实时播放任务频繁进入 printf/UART 锁。 */
#define APP_INTERCOM_GAP_LOG_INTERVAL_MS 1000u
/** @brief NACK payload 长度：source_device[16] + channel + start_seq + count。 */
#define APP_INTERCOM_NACK_PAYLOAD_LEN    (APP_INTERCOM_DEVICE_FIELD_LEN + 8u)
/** @brief 单个 NACK 最多请求的连续缺包数，FEC 为主后避免补发突发过大。 */
#define APP_INTERCOM_NACK_MAX_COUNT      8u
/** @brief 播放中只请求距离播放点至少这么多包的缺口，太近的补发来不及。 */
#define APP_INTERCOM_NACK_MIN_AHEAD      20u
/** @brief 同一个缺口重复请求的最小间隔。 */
#define APP_INTERCOM_NACK_REPEAT_MS      300u
/** @brief NACK 请求日志节流。 */
#define APP_INTERCOM_NACK_LOG_INTERVAL_MS 1000u
/** @brief WebSocket 下行接收任务栈大小，只做 TCP/WebSocket 读包和入队。 */
#define APP_INTERCOM_WS_TASK_STACK       8192u
/** @brief WebSocket 断线后的重连间隔，单位 ms。 */
#define APP_INTERCOM_WS_RECONNECT_MS     2000u
/** @brief WebSocket 空闲 ping 间隔，维持长连接可用性。 */
#define APP_INTERCOM_WS_PING_MS          15000u
/** @brief WebSocket 握手响应超时，公网/热点链路下 500ms 过短。 */
#define APP_INTERCOM_WS_HANDSHAKE_TIMEOUT_MS 5000u
/** @brief WebSocket 运行态读取超时，避免任务永久卡住无法感知断网。 */
#define APP_INTERCOM_WS_RECV_TIMEOUT_MS  1000u
/** @brief WebSocket HTTP 握手响应缓冲大小。 */
#define APP_INTERCOM_WS_HANDSHAKE_BYTES  512u
/** @brief WebSocket 下行队列容量，32 包约 640ms。满时丢最旧包防止播放历史积压。 */
#define APP_INTERCOM_WS_RX_QUEUE_LEN     32u
/** @brief WebSocket 下行队列丢包日志节流。 */
#define APP_INTERCOM_WS_DROP_LOG_MS      1000u
/** @brief RFC 示例 key；设备端只校验 101 状态，不依赖 key 的随机性。 */
#define APP_INTERCOM_WS_CLIENT_KEY       "dGhlIHNhbXBsZSBub25jZQ=="

#define APP_INTERCOM_WS_OPCODE_CONT      0x0u
#define APP_INTERCOM_WS_OPCODE_TEXT      0x1u
#define APP_INTERCOM_WS_OPCODE_BINARY    0x2u
#define APP_INTERCOM_WS_OPCODE_CLOSE     0x8u
#define APP_INTERCOM_WS_OPCODE_PING      0x9u
#define APP_INTERCOM_WS_OPCODE_PONG      0xau

/** @brief 自定义应用层协议包类型枚举。 */
typedef enum {
    APP_INTERCOM_PKT_REGISTER = 1,  /**< 设备注册（上报设备名到服务器） */
    APP_INTERCOM_PKT_CHANNEL = 2,   /**< 频道切换 */
    APP_INTERCOM_PKT_PTT_START = 3, /**< PTT 开始（对讲键按下） */
    APP_INTERCOM_PKT_AUDIO = 4,     /**< 音频数据帧（20ms PCM） */
    APP_INTERCOM_PKT_PTT_STOP = 5,  /**< PTT 结束（对讲键松开） */
    APP_INTERCOM_PKT_HEARTBEAT = 6, /**< 心跳保活（空闲 3s 间隔） */
    APP_INTERCOM_PKT_NACK = 7,      /**< 接收端机会型补洞请求。 */
    APP_INTERCOM_PKT_AUDIO_FEC = 8, /**< 下行 AUDIO XOR 前向纠错包。 */
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
    int16_t pcm[APP_INTERCOM_PACKET_SAMPLES];      /**< 固定 20ms PCM 包。 */
} app_intercom_jitter_frame_t;

typedef struct {
    uint16_t len;                                  /**< 完整 WTK1 包长度。 */
    uint8_t data[APP_INTERCOM_PACKET_MAX_BYTES];   /**< 完整 WTK1 包字节。 */
} app_intercom_ws_rx_frame_t;

/* ==========================================================================
 * 全局状态变量
 * ========================================================================== */

/** @brief PTT 发送任务句柄，优先级 6（高于 biz_ai），
 *  确保 PTT 实时音频采集不被 AI 任务抢占。 */
static osal_task_t s_ptt_task = NULL;

/** @brief 心跳/重连任务句柄，网络切换后用于立即唤醒重连。 */
static osal_task_t s_heartbeat_task = NULL;

/** @brief WebSocket 下行长连接任务句柄。 */
static osal_task_t s_ws_task = NULL;

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
static int16_t s_rx_pcm[APP_INTERCOM_PACKET_SAMPLES];
/** @brief UDP 接收上一帧 PCM，用于缺包补偿。 */
static int16_t s_rx_last_pcm[APP_INTERCOM_PACKET_SAMPLES];
/** @brief FEC 恢复出的 PCM payload 临时缓冲，单 UDP RX 任务独占。 */
static uint8_t s_rx_fec_pcm[APP_INTERCOM_AUDIO_PAYLOAD_BYTES];
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
/** @brief 当前自适应起播水位，网络差时临时提高以减少卡顿。 */
static uint8_t s_rx_jitter_target_start = APP_INTERCOM_JITTER_START_FRAMES;
/** @brief 连续稳定播放帧数，用于恢复低延迟水位。 */
static uint16_t s_rx_jitter_stable_frames = 0u;
/** @brief UDP RX 统计日志节流时间。 */
static uint32_t s_rx_stat_log_ms = 0u;
/** @brief UDP RX 缺口跳帧日志节流时间。 */
static uint32_t s_rx_gap_log_ms = 0u;
/** @brief UDP RX 最近收到的音频序列号。 */
static uint32_t s_rx_stat_last_seq = 0u;
/** @brief UDP RX 是否已有上一帧序列号。 */
static uint8_t s_rx_stat_has_last_seq = 0u;
/** @brief UDP RX 本统计窗口收到的音频包数。 */
static uint32_t s_rx_stat_audio = 0u;
/** @brief UDP RX 本统计窗口发生的序列缺口次数。 */
static uint32_t s_rx_stat_gap_events = 0u;
/** @brief UDP RX 本统计窗口累计缺失的序列帧数。 */
static uint32_t s_rx_stat_gap_frames = 0u;
/** @brief UDP RX 本统计窗口收到但已晚于播放位置的包数。 */
static uint32_t s_rx_stat_late = 0u;
/** @brief UDP RX 本统计窗口重复包数。 */
static uint32_t s_rx_stat_duplicate = 0u;
/** @brief UDP RX 本统计窗口覆盖 jitter 槽位的次数。 */
static uint32_t s_rx_stat_overwrite = 0u;
/** @brief UDP RX 本统计窗口太超前导致播放指针前移的次数。 */
static uint32_t s_rx_stat_far_ahead = 0u;
/** @brief UDP RX 本统计窗口发送 NACK 请求次数。 */
static uint32_t s_rx_stat_nack = 0u;
/** @brief UDP RX 本统计窗口收到的 FEC 包数。 */
static uint32_t s_rx_stat_fec = 0u;
/** @brief UDP RX 本统计窗口通过 FEC 恢复的 AUDIO 包数。 */
static uint32_t s_rx_stat_fec_recovered = 0u;
/** @brief UDP RX 本统计窗口无法使用的 FEC 包数。 */
static uint32_t s_rx_stat_fec_skip = 0u;
/** @brief 最近一次 NACK 请求的来源设备名，用于节流重复请求。 */
static char s_rx_last_nack_device[APP_INTERCOM_DEVICE_FIELD_LEN + 1u];
/** @brief 最近一次 NACK 请求的起始序列号。 */
static uint32_t s_rx_last_nack_start_seq = 0u;
/** @brief 最近一次 NACK 请求的时间。 */
static uint32_t s_rx_last_nack_ms = 0u;
/** @brief NACK 请求日志节流时间。 */
static uint32_t s_rx_nack_log_ms = 0u;
/** @brief FEC 恢复日志节流时间。 */
static uint32_t s_rx_fec_log_ms = 0u;

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

/** @brief WebSocket 下行是否已经握手成功。 */
static volatile int s_ws_connected = 0;
/** @brief 网络切换时请求 WebSocket 任务关闭旧 socket 并重连。 */
static volatile int s_ws_force_reconnect = 0;
/** @brief WebSocket 断线后请求 RX 任务清空旧播放缓存。 */
static volatile int s_ws_reset_rx = 0;
/** @brief WebSocket 下行队列互斥锁。 */
static osal_mutex_t s_ws_rx_mutex = NULL;
/** @brief WebSocket binary 下行队列，保存完整 WTK1 包，运行时分配到 PSRAM。 */
static app_intercom_ws_rx_frame_t *s_ws_rx_ring = NULL;
/** @brief WebSocket 下行队列写位置。 */
static uint8_t s_ws_rx_head = 0u;
/** @brief WebSocket 下行队列读位置。 */
static uint8_t s_ws_rx_tail = 0u;
/** @brief WebSocket 下行队列当前包数。 */
static uint8_t s_ws_rx_count = 0u;
/** @brief WebSocket 下行队列满时丢弃的旧包数量。 */
static uint32_t s_ws_rx_drop_count = 0u;
/** @brief WebSocket 下行队列丢包日志节流时间。 */
static uint32_t s_ws_rx_drop_log_ms = 0u;
/** @brief WebSocket RX 任务弹出包时的临时缓冲，避免覆盖 UDP 半包缓冲。 */
static uint8_t s_ws_rx_packet_buf[APP_INTERCOM_PACKET_MAX_BYTES];

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
    strncpy((char *)&out[16], APP_DEVICE_ID, APP_INTERCOM_DEVICE_FIELD_LEN - 1u);
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
    return strncmp(name, APP_DEVICE_ID, APP_INTERCOM_DEVICE_FIELD_LEN) == 0;
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

static int app_intercom_send_nack(const char *source_device,
                                  uint32_t start_seq,
                                  uint16_t count,
                                  uint32_t now)
{
    uint8_t payload[APP_INTERCOM_NACK_PAYLOAD_LEN];
    uint8_t packet[APP_INTERCOM_PACKET_HEADER_LEN + APP_INTERCOM_NACK_PAYLOAD_LEN];

    if (!s_udp_ready ||
        s_ws_connected != 0 ||
        source_device == NULL ||
        source_device[0] == '\0' ||
        count == 0u) {
        return -1;
    }

    if (count > APP_INTERCOM_NACK_MAX_COUNT) {
        count = APP_INTERCOM_NACK_MAX_COUNT;
    }

    memset(payload, 0, sizeof(payload));
    strncpy((char *)&payload[0], source_device, APP_INTERCOM_DEVICE_FIELD_LEN - 1u);
    app_intercom_write_u16(&payload[APP_INTERCOM_DEVICE_FIELD_LEN], (uint16_t)s_current_channel);
    app_intercom_write_u32(&payload[APP_INTERCOM_DEVICE_FIELD_LEN + 2u], start_seq);
    app_intercom_write_u16(&payload[APP_INTERCOM_DEVICE_FIELD_LEN + 6u], count);

    uint16_t len = app_intercom_build_packet(packet,
                                             APP_INTERCOM_PKT_NACK,
                                             payload,
                                             (uint16_t)sizeof(payload));
    if (len == 0u) {
        return -2;
    }

    int ret = service_network_udp_send(packet, len);
    if (ret == 0) {
        s_rx_stat_nack++;
        if ((uint32_t)(now - s_rx_nack_log_ms) >= APP_INTERCOM_NACK_LOG_INTERVAL_MS) {
            APP_LOGI(TAG,
                     "UDP NACK请求: source=%s, ch=%u, start=%u, count=%u",
                     source_device,
                     (unsigned int)s_current_channel,
                     (unsigned int)start_seq,
                     (unsigned int)count);
            s_rx_nack_log_ms = now;
        }
    }
    return ret;
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

static int app_intercom_ws_rx_init(void)
{
    if (s_ws_rx_ring == NULL) {
        size_t bytes = sizeof(app_intercom_ws_rx_frame_t) * APP_INTERCOM_WS_RX_QUEUE_LEN;
        s_ws_rx_ring = (app_intercom_ws_rx_frame_t *)heap_caps_malloc(bytes,
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ws_rx_ring == NULL) {
            APP_LOGE(TAG,
                     "WebSocket 下行队列 PSRAM 分配失败, bytes=%u, psram_free=%u, internal_largest=%u",
                     (unsigned int)bytes,
                     (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return -1;
        }
    }

    if (s_ws_rx_mutex == NULL) {
        s_ws_rx_mutex = osal_mutex_create();
        if (s_ws_rx_mutex == NULL) {
            APP_LOGE(TAG, "WebSocket 下行队列初始化失败");
            return -2;
        }
    }
    return 0;
}

static void app_intercom_ws_rx_clear(void)
{
    if (s_ws_rx_mutex == NULL ||
        osal_mutex_lock(s_ws_rx_mutex, OSAL_WAIT_FOREVER) != 0) {
        return;
    }

    s_ws_rx_head = 0u;
    s_ws_rx_tail = 0u;
    s_ws_rx_count = 0u;
    osal_mutex_unlock(s_ws_rx_mutex);
}

static void app_intercom_ws_rx_push(const uint8_t *data, uint16_t len)
{
    if (data == NULL ||
        len < APP_INTERCOM_PACKET_HEADER_LEN ||
        len > APP_INTERCOM_PACKET_MAX_BYTES ||
        s_ws_rx_ring == NULL ||
        s_ws_rx_mutex == NULL) {
        return;
    }

    if (osal_mutex_lock(s_ws_rx_mutex, OSAL_WAIT_NONE) != 0) {
        return;
    }

    if (s_ws_rx_count >= APP_INTERCOM_WS_RX_QUEUE_LEN) {
        s_ws_rx_tail = (uint8_t)((s_ws_rx_tail + 1u) % APP_INTERCOM_WS_RX_QUEUE_LEN);
        s_ws_rx_count--;
        s_ws_rx_drop_count++;

        uint32_t now = osal_get_tick_ms();
        if ((uint32_t)(now - s_ws_rx_drop_log_ms) >= APP_INTERCOM_WS_DROP_LOG_MS) {
            APP_LOGW(TAG,
                     "WebSocket 下行队列丢弃旧音频, drop=%u, queue=%u",
                     (unsigned int)s_ws_rx_drop_count,
                     (unsigned int)s_ws_rx_count);
            s_ws_rx_drop_log_ms = now;
        }
    }

    app_intercom_ws_rx_frame_t *frame = &s_ws_rx_ring[s_ws_rx_head];
    frame->len = len;
    memcpy(frame->data, data, len);
    s_ws_rx_head = (uint8_t)((s_ws_rx_head + 1u) % APP_INTERCOM_WS_RX_QUEUE_LEN);
    s_ws_rx_count++;

    osal_mutex_unlock(s_ws_rx_mutex);
}

static int app_intercom_ws_rx_pop(uint8_t *out, uint16_t *out_len)
{
    if (out == NULL || out_len == NULL || s_ws_rx_ring == NULL || s_ws_rx_mutex == NULL) {
        return -1;
    }

    if (osal_mutex_lock(s_ws_rx_mutex, OSAL_WAIT_NONE) != 0) {
        return -2;
    }

    if (s_ws_rx_count == 0u) {
        osal_mutex_unlock(s_ws_rx_mutex);
        return -3;
    }

    const app_intercom_ws_rx_frame_t *frame = &s_ws_rx_ring[s_ws_rx_tail];
    *out_len = frame->len;
    memcpy(out, frame->data, frame->len);
    s_ws_rx_tail = (uint8_t)((s_ws_rx_tail + 1u) % APP_INTERCOM_WS_RX_QUEUE_LEN);
    s_ws_rx_count--;

    osal_mutex_unlock(s_ws_rx_mutex);
    return 0;
}

static int app_intercom_ws_send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0u;
    while (sent < len) {
        int ret = send(sock, &data[sent], len - sent, 0);
        if (ret > 0) {
            sent += (size_t)ret;
            continue;
        }
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

static int app_intercom_ws_recv_exact(int sock, uint8_t *buf, size_t len, int allow_idle)
{
    size_t got = 0u;
    while (got < len) {
        int ret = recv(sock, &buf[got], len - got, 0);
        if (ret > 0) {
            got += (size_t)ret;
            continue;
        }
        if (ret == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && got == 0u && allow_idle != 0) {
            return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (service_network_is_ready() != 1) {
                return -2;
            }
            continue;
        }
        return -3;
    }

    return 0;
}

static int app_intercom_ws_drain_payload(int sock, uint64_t len)
{
    uint8_t tmp[64];
    while (len > 0u) {
        size_t take = len > sizeof(tmp) ? sizeof(tmp) : (size_t)len;
        int ret = app_intercom_ws_recv_exact(sock, tmp, take, 0);
        if (ret != 0) {
            return -1;
        }
        len -= take;
    }

    return 0;
}

static int app_intercom_ws_set_recv_timeout(int sock, uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };

    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 ? 0 : -1;
}

static int app_intercom_ws_send_control_frame(int sock,
                                              uint8_t opcode,
                                              const uint8_t *payload,
                                              uint8_t payload_len)
{
    uint8_t frame[2u + 4u + 125u];
    uint8_t mask[4];
    uint32_t seed = osal_get_tick_ms();

    if (payload_len > 125u || (payload_len > 0u && payload == NULL)) {
        return -1;
    }

    mask[0] = (uint8_t)(seed & 0xffu);
    mask[1] = (uint8_t)((seed >> 8) & 0xffu);
    mask[2] = (uint8_t)((seed >> 16) & 0xffu);
    mask[3] = (uint8_t)((seed >> 24) & 0xffu);

    frame[0] = (uint8_t)(0x80u | (opcode & 0x0fu));
    frame[1] = (uint8_t)(0x80u | payload_len);
    memcpy(&frame[2], mask, sizeof(mask));
    for (uint8_t i = 0u; i < payload_len; i++) {
        frame[6u + i] = payload[i] ^ mask[i % 4u];
    }

    return app_intercom_ws_send_all(sock, frame, (size_t)(6u + payload_len));
}

static int app_intercom_ws_connect_socket(void)
{
    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    snprintf(port_str, sizeof(port_str), "%d", APP_BUSINESS_WS_PORT);
    if (getaddrinfo(APP_BUSINESS_SERVER_HOST, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        freeaddrinfo(res);
        return -2;
    }

    (void)app_intercom_ws_set_recv_timeout(sock, APP_INTERCOM_WS_HANDSHAKE_TIMEOUT_MS);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret != 0) {
        close(sock);
        return -3;
    }

    return sock;
}

static int app_intercom_ws_handshake(int sock)
{
    char req[384];
    char resp[APP_INTERCOM_WS_HANDSHAKE_BYTES];
    int used = 0;

    int len = snprintf(req,
                       sizeof(req),
                       "GET %s?device=%s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "\r\n",
                       APP_BUSINESS_WS_ROUTE_INTERCOM,
                       APP_DEVICE_ID,
                       APP_BUSINESS_SERVER_HOST,
                       APP_BUSINESS_WS_PORT,
                       APP_INTERCOM_WS_CLIENT_KEY);
    if (len <= 0 || len >= (int)sizeof(req)) {
        return -1;
    }

    if (app_intercom_ws_send_all(sock, (const uint8_t *)req, (size_t)len) != 0) {
        return -2;
    }

    memset(resp, 0, sizeof(resp));
    while (used < ((int)sizeof(resp) - 1)) {
        int ret = recv(sock, &resp[used], (size_t)((int)sizeof(resp) - 1 - used), 0);
        if (ret > 0) {
            used += ret;
            resp[used] = '\0';
            if (strstr(resp, "\r\n\r\n") != NULL) {
                break;
            }
            continue;
        }
        if (ret == 0) {
            return -3;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -4;
        }
        return -5;
    }

    if (strstr(resp, " 101 ") == NULL && strstr(resp, " 101\r\n") == NULL) {
        return -6;
    }

    return 0;
}

static int app_intercom_ws_recv_frame(int sock, uint8_t *payload, uint16_t *payload_len)
{
    uint8_t hdr[2];
    uint8_t mask[4] = {0};
    int ret = app_intercom_ws_recv_exact(sock, hdr, sizeof(hdr), 1);
    if (ret != 0) {
        return ret;
    }

    uint8_t opcode = hdr[0] & 0x0fu;
    uint8_t masked = (hdr[1] & 0x80u) != 0u ? 1u : 0u;
    uint64_t len = hdr[1] & 0x7fu;
    if (len == 126u) {
        uint8_t ext[2];
        ret = app_intercom_ws_recv_exact(sock, ext, sizeof(ext), 0);
        if (ret != 0) {
            return -2;
        }
        len = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
    } else if (len == 127u) {
        uint8_t ext[8];
        ret = app_intercom_ws_recv_exact(sock, ext, sizeof(ext), 0);
        if (ret != 0) {
            return -3;
        }
        len = 0u;
        for (uint8_t i = 0u; i < 8u; i++) {
            len = (len << 8) | (uint64_t)ext[i];
        }
    }

    if (masked != 0u) {
        ret = app_intercom_ws_recv_exact(sock, mask, sizeof(mask), 0);
        if (ret != 0) {
            return -4;
        }
    }

    if (opcode == APP_INTERCOM_WS_OPCODE_CLOSE) {
        if (len > 0u) {
            (void)app_intercom_ws_drain_payload(sock, len);
        }
        (void)app_intercom_ws_send_control_frame(sock, APP_INTERCOM_WS_OPCODE_CLOSE, NULL, 0u);
        return -5;
    }

    if (len > APP_INTERCOM_PACKET_MAX_BYTES) {
        (void)app_intercom_ws_drain_payload(sock, len);
        return 1;
    }

    uint16_t frame_len = (uint16_t)len;
    ret = app_intercom_ws_recv_exact(sock, payload, frame_len, 0);
    if (ret != 0) {
        return -6;
    }

    if (masked != 0u) {
        for (uint16_t i = 0u; i < frame_len; i++) {
            payload[i] ^= mask[i % 4u];
        }
    }

    if (opcode == APP_INTERCOM_WS_OPCODE_PING) {
        (void)app_intercom_ws_send_control_frame(sock,
                                                 APP_INTERCOM_WS_OPCODE_PONG,
                                                 payload,
                                                 (uint8_t)frame_len);
        return 1;
    }
    if (opcode == APP_INTERCOM_WS_OPCODE_PONG ||
        opcode == APP_INTERCOM_WS_OPCODE_TEXT ||
        opcode == APP_INTERCOM_WS_OPCODE_CONT) {
        return 1;
    }
    if (opcode != APP_INTERCOM_WS_OPCODE_BINARY) {
        return 1;
    }

    *payload_len = frame_len;
    return 0;
}

static void app_intercom_ws_task(void *arg)
{
    (void)arg;
    uint8_t packet[APP_INTERCOM_PACKET_MAX_BYTES];

    while (1) {
        if (service_network_is_ready() != 1) {
            if (s_ws_connected != 0) {
                s_ws_connected = 0;
                s_ws_reset_rx = 1;
                app_intercom_ws_rx_clear();
            }
            s_ws_force_reconnect = 0;
            osal_delay_ms(APP_INTERCOM_WS_RECONNECT_MS);
            continue;
        }

        s_ws_force_reconnect = 0;
        int sock = app_intercom_ws_connect_socket();
        if (sock < 0) {
            APP_LOGW(TAG, "WebSocket 下行连接失败, ret=%d", sock);
            osal_delay_ms(APP_INTERCOM_WS_RECONNECT_MS);
            continue;
        }

        int ret = app_intercom_ws_handshake(sock);
        if (ret != 0) {
            APP_LOGW(TAG, "WebSocket 下行握手失败, ret=%d", ret);
            close(sock);
            osal_delay_ms(APP_INTERCOM_WS_RECONNECT_MS);
            continue;
        }
        (void)app_intercom_ws_set_recv_timeout(sock, APP_INTERCOM_WS_RECV_TIMEOUT_MS);

        s_ws_connected = 1;
        s_ws_reset_rx = 0;
        app_intercom_ws_rx_clear();
        APP_LOGI(TAG,
                 "WebSocket 下行已连接: ws://%s:%d%s?device=%s",
                 APP_BUSINESS_SERVER_HOST,
                 APP_BUSINESS_WS_PORT,
                 APP_BUSINESS_WS_ROUTE_INTERCOM,
                 APP_DEVICE_ID);

        uint32_t last_ping_ms = osal_get_tick_ms();
        while (service_network_is_ready() == 1 && s_ws_force_reconnect == 0) {
            uint16_t packet_len = 0u;
            ret = app_intercom_ws_recv_frame(sock, packet, &packet_len);
            if (ret == 0) {
                app_intercom_ws_rx_push(packet, packet_len);
                continue;
            }
            if (ret < 0) {
                break;
            }

            uint32_t now = osal_get_tick_ms();
            if ((uint32_t)(now - last_ping_ms) >= APP_INTERCOM_WS_PING_MS) {
                uint8_t ping_payload[2] = {'w', 's'};
                if (app_intercom_ws_send_control_frame(sock,
                                                       APP_INTERCOM_WS_OPCODE_PING,
                                                       ping_payload,
                                                       (uint8_t)sizeof(ping_payload)) != 0) {
                    break;
                }
                last_ping_ms = now;
            }
        }

        close(sock);
        s_ws_connected = 0;
        s_ws_force_reconnect = 0;
        s_ws_reset_rx = 1;
        app_intercom_ws_rx_clear();
        APP_LOGW(TAG, "WebSocket 下行断开，准备重连");
        osal_delay_ms(APP_INTERCOM_WS_RECONNECT_MS);
    }
}

static int app_intercom_start_ws_downlink_task(void)
{
    if (APP_INTERCOM_USE_WS_DOWNLINK == 0) {
        return 0;
    }
    if (s_ws_task != NULL) {
        return 0;
    }
    if (app_intercom_ws_rx_init() != 0) {
        return -1;
    }

    TaskHandle_t handle = NULL;
    BaseType_t ret = xTaskCreateWithCaps(app_intercom_ws_task,
                                         "biz_ws_rx",
                                         APP_INTERCOM_WS_TASK_STACK,
                                         NULL,
                                         5u,
                                         &handle,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        APP_LOGW(TAG,
                 "WebSocket 下行任务启动失败, ret=%d, psram_free=%u, internal_free=%u, internal_largest=%u",
                 (int)ret,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return -2;
    }

    s_ws_task = (osal_task_t)handle;
    return 0;
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

static int app_intercom_audio_busy(void)
{
    return (s_ptt_active != 0 ||
            s_rx_jitter_playing != 0u ||
            s_rx_playback_active != 0) ? 1 : 0;
}

static int app_intercom_seq_before(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b)) < 0;
}

static uint8_t app_intercom_jitter_count_ready(void);

static void app_intercom_nack_reset(void)
{
    s_rx_last_nack_device[0] = '\0';
    s_rx_last_nack_start_seq = 0u;
    s_rx_last_nack_ms = 0u;
}

static void app_intercom_maybe_request_nack(const app_intercom_packet_view_t *view,
                                            uint32_t start_seq,
                                            uint16_t count,
                                            uint32_t now)
{
    if (view == NULL || count == 0u) {
        return;
    }
    if (count > APP_INTERCOM_NACK_MAX_COUNT) {
        count = APP_INTERCOM_NACK_MAX_COUNT;
    }
    if (app_intercom_seq_before(start_seq, s_rx_jitter_expected_seq)) {
        return;
    }

    /*
     * 播放中只请求足够靠前的洞；未起播时还在攒缓存，早期缺口也有机会补上。
     * 播放绝不等待 NACK 回包，赶不上就继续走现有跳帧/PLC。
     */
    if (s_rx_jitter_playing != 0u) {
        uint32_t ahead = start_seq - s_rx_jitter_expected_seq;
        if (ahead < APP_INTERCOM_NACK_MIN_AHEAD) {
            return;
        }
    }

    if (s_rx_last_nack_device[0] != '\0' &&
        strncmp(s_rx_last_nack_device, view->device, APP_INTERCOM_DEVICE_FIELD_LEN) == 0 &&
        s_rx_last_nack_start_seq == start_seq &&
        (uint32_t)(now - s_rx_last_nack_ms) < APP_INTERCOM_NACK_REPEAT_MS) {
        return;
    }

    if (app_intercom_send_nack(view->device, start_seq, count, now) == 0) {
        strncpy(s_rx_last_nack_device, view->device, sizeof(s_rx_last_nack_device) - 1u);
        s_rx_last_nack_device[sizeof(s_rx_last_nack_device) - 1u] = '\0';
        s_rx_last_nack_start_seq = start_seq;
        s_rx_last_nack_ms = now;
    }
}

static void app_intercom_nack_note_audio_gap(const app_intercom_packet_view_t *view, uint32_t now)
{
    if (view == NULL || s_rx_stat_has_last_seq == 0u) {
        return;
    }
    if (view->seq == s_rx_stat_last_seq ||
        app_intercom_seq_before(view->seq, s_rx_stat_last_seq)) {
        return;
    }

    uint32_t expected_seq = s_rx_stat_last_seq + 1u;
    if (view->seq == expected_seq) {
        return;
    }

    uint32_t missing = view->seq - expected_seq;
    app_intercom_maybe_request_nack(view,
                                    expected_seq,
                                    (uint16_t)(missing > UINT16_MAX ? UINT16_MAX : missing),
                                    now);
}

static void app_intercom_rx_stats_reset(uint32_t first_seq)
{
    s_rx_stat_log_ms = osal_get_tick_ms();
    s_rx_stat_last_seq = first_seq;
    s_rx_stat_has_last_seq = 0u;
    s_rx_stat_audio = 0u;
    s_rx_stat_gap_events = 0u;
    s_rx_stat_gap_frames = 0u;
    s_rx_stat_late = 0u;
    s_rx_stat_duplicate = 0u;
    s_rx_stat_overwrite = 0u;
    s_rx_stat_far_ahead = 0u;
    s_rx_stat_nack = 0u;
    s_rx_stat_fec = 0u;
    s_rx_stat_fec_recovered = 0u;
    s_rx_stat_fec_skip = 0u;
}

static void app_intercom_rx_stats_note_audio(uint32_t seq)
{
    s_rx_stat_audio++;
    if (s_rx_stat_has_last_seq == 0u) {
        s_rx_stat_last_seq = seq;
        s_rx_stat_has_last_seq = 1u;
        return;
    }

    if (seq == s_rx_stat_last_seq) {
        s_rx_stat_duplicate++;
        return;
    }

    if (app_intercom_seq_before(seq, s_rx_stat_last_seq)) {
        s_rx_stat_late++;
        return;
    }

    uint32_t expected_seq = s_rx_stat_last_seq + 1u;
    if (seq != expected_seq) {
        s_rx_stat_gap_events++;
        s_rx_stat_gap_frames += seq - expected_seq;
    }
    s_rx_stat_last_seq = seq;
}

static void app_intercom_rx_stats_log(uint32_t now)
{
    if (s_rx_stat_audio == 0u ||
        (uint32_t)(now - s_rx_stat_log_ms) < 1000u) {
        return;
    }

    APP_LOGI(TAG,
             "UDP RX统计: device=%s, rx=%u, gap=%u/%u, late=%u, dup=%u, overwrite=%u, far=%u, nack=%u, fec=%u/%u/%u, expected=%u, last_rx=%u, buffered=%u",
             s_rx_jitter_device,
             (unsigned int)s_rx_stat_audio,
             (unsigned int)s_rx_stat_gap_events,
             (unsigned int)s_rx_stat_gap_frames,
             (unsigned int)s_rx_stat_late,
             (unsigned int)s_rx_stat_duplicate,
             (unsigned int)s_rx_stat_overwrite,
             (unsigned int)s_rx_stat_far_ahead,
             (unsigned int)s_rx_stat_nack,
             (unsigned int)s_rx_stat_fec,
             (unsigned int)s_rx_stat_fec_recovered,
             (unsigned int)s_rx_stat_fec_skip,
             (unsigned int)s_rx_jitter_expected_seq,
             (unsigned int)s_rx_stat_last_seq,
             (unsigned int)app_intercom_jitter_count_ready());

    s_rx_stat_log_ms = now;
    s_rx_stat_audio = 0u;
    s_rx_stat_gap_events = 0u;
    s_rx_stat_gap_frames = 0u;
    s_rx_stat_late = 0u;
    s_rx_stat_duplicate = 0u;
    s_rx_stat_overwrite = 0u;
    s_rx_stat_far_ahead = 0u;
    s_rx_stat_nack = 0u;
    s_rx_stat_fec = 0u;
    s_rx_stat_fec_recovered = 0u;
    s_rx_stat_fec_skip = 0u;
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
    app_intercom_nack_reset();
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

static uint8_t app_intercom_jitter_start_frames(void)
{
    if (s_rx_jitter_target_start < APP_INTERCOM_JITTER_START_FRAMES) {
        return APP_INTERCOM_JITTER_START_FRAMES;
    }
    if (s_rx_jitter_target_start > APP_INTERCOM_JITTER_MAX_START_FRAMES) {
        return APP_INTERCOM_JITTER_MAX_START_FRAMES;
    }
    return s_rx_jitter_target_start;
}

static uint8_t app_intercom_jitter_low_water(void)
{
    uint8_t target_start = app_intercom_jitter_start_frames();
    if (target_start <= 3u) {
        return APP_INTERCOM_JITTER_LOW_WATER;
    }

    uint8_t adaptive_low = (uint8_t)(target_start - 3u);
    return adaptive_low > APP_INTERCOM_JITTER_LOW_WATER ?
           adaptive_low :
           APP_INTERCOM_JITTER_LOW_WATER;
}

static void app_intercom_jitter_bump_target(void)
{
    s_rx_jitter_stable_frames = 0u;
    if (s_rx_jitter_target_start < APP_INTERCOM_JITTER_MAX_START_FRAMES) {
        s_rx_jitter_target_start++;
        APP_LOGW(TAG, "UDP 音频 jitter 提高缓存水位, target=%u",
                 (unsigned int)s_rx_jitter_target_start);
    }
}

static void app_intercom_jitter_recover_target(void)
{
    if (s_rx_jitter_target_start <= APP_INTERCOM_JITTER_START_FRAMES) {
        s_rx_jitter_stable_frames = 0u;
        return;
    }

    s_rx_jitter_stable_frames++;
    if (s_rx_jitter_stable_frames >= APP_INTERCOM_JITTER_RECOVER_FRAMES) {
        s_rx_jitter_target_start--;
        s_rx_jitter_stable_frames = 0u;
        APP_LOGI(TAG, "UDP 音频 jitter 降低缓存水位, target=%u",
                 (unsigned int)s_rx_jitter_target_start);
    }
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
    app_intercom_rx_stats_reset(seq);
}

static void app_intercom_jitter_enqueue_pcm(const app_intercom_packet_view_t *view,
                                            const uint8_t *pcm,
                                            uint16_t samples)
{
    if (view == NULL || pcm == NULL || samples == 0u) {
        return;
    }
    if (samples > APP_INTERCOM_PACKET_SAMPLES) {
        samples = APP_INTERCOM_PACKET_SAMPLES;
    }

    if (s_rx_jitter_ready == 0u ||
        strncmp(s_rx_jitter_device, view->device, APP_INTERCOM_DEVICE_FIELD_LEN) != 0) {
        app_intercom_jitter_reset_for_source(view->device, view->seq);
    }

    uint32_t now = osal_get_tick_ms();
    app_intercom_nack_note_audio_gap(view, now);
    app_intercom_rx_stats_note_audio(view->seq);

    if (s_rx_jitter_playing && app_intercom_seq_before(view->seq, s_rx_jitter_expected_seq)) {
        s_rx_stat_late++;
        app_intercom_rx_stats_log(now);
        return;
    }

    uint32_t ahead = view->seq - s_rx_jitter_expected_seq;
    if (ahead >= APP_INTERCOM_JITTER_FRAME_COUNT) {
        s_rx_stat_far_ahead++;
        s_rx_jitter_expected_seq = view->seq - (APP_INTERCOM_JITTER_FRAME_COUNT - 1u);
        app_intercom_jitter_drop_before(s_rx_jitter_expected_seq);
    }

    uint32_t slot_index = view->seq % APP_INTERCOM_JITTER_FRAME_COUNT;
    app_intercom_jitter_frame_t *frame = &s_rx_jitter[slot_index];
    if (frame->valid && frame->seq == view->seq) {
        s_rx_stat_duplicate++;
        app_intercom_rx_stats_log(now);
        return;
    }
    if (frame->valid && frame->seq != view->seq) {
        s_rx_stat_overwrite++;
    }

    memset(frame->pcm, 0, sizeof(frame->pcm));
    memcpy(frame->pcm, pcm, (size_t)samples * sizeof(int16_t));
    frame->samples = samples;
    frame->seq = view->seq;
    frame->valid = 1u;
    s_rx_jitter_last_enqueue_ms = now;
    app_intercom_rx_stats_log(now);
}

static void app_intercom_jitter_enqueue(const app_intercom_packet_view_t *view)
{
    if (view == NULL || view->payload == NULL || view->payload_len < sizeof(int16_t)) {
        return;
    }

    uint16_t samples = (uint16_t)(view->payload_len / sizeof(int16_t));
    app_intercom_jitter_enqueue_pcm(view, view->payload, samples);
}

static int app_intercom_jitter_insert_recovered(const char *device,
                                                uint32_t seq,
                                                const uint8_t *pcm,
                                                uint16_t payload_len,
                                                uint32_t now)
{
    if (device == NULL || pcm == NULL || payload_len != APP_INTERCOM_AUDIO_PAYLOAD_BYTES) {
        return -1;
    }
    if (s_rx_jitter_ready == 0u ||
        strncmp(s_rx_jitter_device, device, APP_INTERCOM_DEVICE_FIELD_LEN) != 0) {
        return -2;
    }
    if (s_rx_jitter_playing != 0u && app_intercom_seq_before(seq, s_rx_jitter_expected_seq)) {
        return -3;
    }

    uint32_t ahead = seq - s_rx_jitter_expected_seq;
    if (ahead >= APP_INTERCOM_JITTER_FRAME_COUNT) {
        return -4;
    }

    app_intercom_jitter_frame_t *frame = &s_rx_jitter[seq % APP_INTERCOM_JITTER_FRAME_COUNT];
    if (frame->valid) {
        return frame->seq == seq ? 1 : -5;
    }

    memcpy(frame->pcm, pcm, payload_len);
    frame->samples = (uint16_t)(payload_len / sizeof(int16_t));
    frame->seq = seq;
    frame->valid = 1u;
    s_rx_jitter_last_enqueue_ms = now;
    return 0;
}

static void app_intercom_handle_fec_packet(const app_intercom_packet_view_t *view)
{
    if (view == NULL ||
        view->payload == NULL ||
        view->payload_len < APP_INTERCOM_FEC_HEADER_LEN) {
        return;
    }

    s_rx_stat_fec++;
    const uint8_t *payload = view->payload;
    uint32_t base_seq = app_intercom_read_u32(&payload[0]);
    uint8_t count = payload[4];
    uint16_t payload_len = app_intercom_read_u16(&payload[5]);
    if (count == 0u ||
        count > APP_INTERCOM_FEC_GROUP_SIZE ||
        payload_len != APP_INTERCOM_AUDIO_PAYLOAD_BYTES ||
        view->payload_len != (uint16_t)(APP_INTERCOM_FEC_HEADER_LEN + payload_len)) {
        s_rx_stat_fec_skip++;
        return;
    }

    uint32_t now = osal_get_tick_ms();
    if (s_rx_jitter_ready == 0u ||
        strncmp(s_rx_jitter_device, view->device, APP_INTERCOM_DEVICE_FIELD_LEN) != 0) {
        s_rx_stat_fec_skip++;
        return;
    }
    if (app_intercom_seq_before(base_seq + count, s_rx_jitter_expected_seq + 1u)) {
        s_rx_stat_fec_skip++;
        return;
    }

    memcpy(s_rx_fec_pcm, &payload[APP_INTERCOM_FEC_HEADER_LEN], payload_len);
    uint32_t missing_seq = 0u;
    uint8_t missing_count = 0u;

    for (uint8_t i = 0u; i < count; i++) {
        uint32_t seq = base_seq + i;
        app_intercom_jitter_frame_t *frame = app_intercom_jitter_find(seq);
        if (frame == NULL ||
            ((uint32_t)frame->samples * sizeof(int16_t)) != payload_len) {
            missing_seq = seq;
            missing_count++;
            continue;
        }

        const uint8_t *frame_pcm = (const uint8_t *)frame->pcm;
        for (uint32_t j = 0u; j < payload_len; j++) {
            s_rx_fec_pcm[j] ^= frame_pcm[j];
        }
    }

    if (missing_count != 1u) {
        s_rx_stat_fec_skip++;
        return;
    }

    int ret = app_intercom_jitter_insert_recovered(view->device,
                                                   missing_seq,
                                                   s_rx_fec_pcm,
                                                   payload_len,
                                                   now);
    if (ret == 0) {
        s_rx_stat_fec_recovered++;
        if ((uint32_t)(now - s_rx_fec_log_ms) >= APP_INTERCOM_NACK_LOG_INTERVAL_MS) {
            APP_LOGI(TAG,
                     "UDP FEC恢复: source=%s, base=%u, seq=%u, count=%u",
                     view->device,
                     (unsigned int)base_seq,
                     (unsigned int)missing_seq,
                     (unsigned int)count);
            s_rx_fec_log_ms = now;
        }
    } else {
        s_rx_stat_fec_skip++;
    }
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

    for (uint32_t i = 0u; i < APP_INTERCOM_PACKET_SAMPLES; i++) {
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
        uint8_t start_frames = app_intercom_jitter_start_frames();
        if (app_intercom_jitter_count_ready() < start_frames) {
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
        APP_LOGI(TAG, "UDP 音频 jitter 起播, device=%s, seq=%u, target=%u",
                 s_rx_jitter_device,
                 (unsigned int)s_rx_jitter_expected_seq,
                 (unsigned int)start_frames);
    }

    if (app_intercom_seq_before(now, s_rx_jitter_next_play_ms)) {
        return;
    }
    if ((uint32_t)(now - s_rx_jitter_next_play_ms) > 100u) {
        s_rx_jitter_next_play_ms = now;
    }

    app_intercom_jitter_frame_t *frame = app_intercom_jitter_find(s_rx_jitter_expected_seq);
    uint16_t samples = APP_INTERCOM_PACKET_SAMPLES;
    uint8_t played_real_frame = 0u;
    if (frame != NULL) {
        memcpy(s_rx_pcm, frame->pcm, sizeof(s_rx_pcm));
        memcpy(s_rx_last_pcm, frame->pcm, sizeof(s_rx_last_pcm));
        samples = frame->samples;
        frame->valid = 0u;
        s_rx_jitter_missing = 0u;
        played_real_frame = 1u;
    } else {
        s_rx_jitter_missing++;
        app_intercom_jitter_bump_target();
        if (s_rx_jitter_missing >= APP_INTERCOM_JITTER_RESYNC_MISSING) {
            uint32_t next_seq = 0u;
            if (app_intercom_jitter_find_next_seq(s_rx_jitter_expected_seq, &next_seq) != 0 &&
                next_seq != s_rx_jitter_expected_seq) {
                uint32_t skipped = next_seq - s_rx_jitter_expected_seq;
                if ((uint32_t)(now - s_rx_gap_log_ms) >= APP_INTERCOM_GAP_LOG_INTERVAL_MS) {
                    APP_LOGW(TAG, "UDP 音频跳过缺口, device=%s, from=%u, to=%u, skipped=%u, buffered=%u",
                             s_rx_jitter_device,
                             (unsigned int)s_rx_jitter_expected_seq,
                             (unsigned int)next_seq,
                             (unsigned int)skipped,
                             (unsigned int)app_intercom_jitter_count_ready());
                    s_rx_gap_log_ms = now;
                }
                s_rx_jitter_expected_seq = next_seq;
                s_rx_jitter_missing = 0u;
                frame = app_intercom_jitter_find(s_rx_jitter_expected_seq);
                if (frame != NULL) {
                    memcpy(s_rx_pcm, frame->pcm, sizeof(s_rx_pcm));
                    memcpy(s_rx_last_pcm, frame->pcm, sizeof(s_rx_last_pcm));
                    samples = frame->samples;
                    frame->valid = 0u;
                    played_real_frame = 1u;
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
        if (played_real_frame != 0u) {
            app_intercom_jitter_recover_target();
        }
        if (s_rx_last_audio_ms - s_rx_play_log_ms >= 1000u) {
            APP_LOGI(TAG, "UDP 音频播放中, device=%s, seq=%u, samples=%d, buffered=%u, missing=%u, target=%u",
                     s_rx_jitter_device,
                     (unsigned int)s_rx_jitter_expected_seq,
                     played,
                     (unsigned int)app_intercom_jitter_count_ready(),
                     (unsigned int)s_rx_jitter_missing,
                     (unsigned int)app_intercom_jitter_start_frames());
            s_rx_play_log_ms = s_rx_last_audio_ms;
        }
    }

    s_rx_jitter_expected_seq++;
    uint8_t buffered_after = app_intercom_jitter_count_ready();
    if (buffered_after >= APP_INTERCOM_JITTER_HIGH_WATER) {
        s_rx_jitter_next_play_ms += (APP_INTERCOM_AUDIO_FRAME_MS / 2u);
    } else if (buffered_after > 0u && buffered_after <= app_intercom_jitter_low_water()) {
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
 * 2. 空闲时每 3 秒发送 HEARTBEAT 保活
 * 3. 检测网络后端 ready 后自动重连 UDP
 * 4. 重连后重新发送 REGISTER + CHANNEL，恢复在线状态
 *
 * ## 重连机制
 * - 网络切换或 PTT 按下时通过 notify 立即唤醒心跳任务
 * - 心跳任务空闲时每 3s 兜底检查 service_network_is_ready() 和 s_udp_ready
 * - 正在发送或播放音频时不发 heartbeat，避免和音频包抢链路
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
        if (app_intercom_audio_busy() == 0) {
            (void)app_intercom_send_control(APP_INTERCOM_PKT_HEARTBEAT);
            (void)osal_task_notify_take(APP_INTERCOM_HEARTBEAT_IDLE_MS);
        } else {
            (void)osal_task_notify_take(APP_INTERCOM_HEARTBEAT_BUSY_MS);
        }
    }
}

static int app_intercom_send_audio_packet(uint8_t *packet,
                                          const int16_t *pcm,
                                          uint16_t samples)
{
    if (packet == NULL || pcm == NULL || samples == 0u || samples > APP_INTERCOM_PACKET_SAMPLES) {
        return -1;
    }

    uint16_t payload_len = (uint16_t)(samples * sizeof(int16_t));
    uint16_t packet_len = app_intercom_build_packet(packet,
                                                    APP_INTERCOM_PKT_AUDIO,
                                                    (const uint8_t *)pcm,
                                                    payload_len);
    if (packet_len == 0u) {
        return -2;
    }

    return service_network_udp_send(packet, packet_len);
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
 * 如果是有效的音频包，将 PCM 放入 jitter buffer。
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
    } else if (view.type == APP_INTERCOM_PKT_AUDIO_FEC &&
               view.payload_len > 0u &&
               s_ws_connected == 0) {
        app_intercom_handle_fec_packet(&view);
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
 * 1. 维护一个内部接收缓冲区 rx[最大包长 × 2]
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

        if (s_ws_reset_rx != 0) {
            s_ws_reset_rx = 0;
            app_intercom_rx_stop_playback();
            app_intercom_jitter_clear();
            used = 0u;
        }

        uint16_t ws_packet_len = 0u;
        uint8_t ws_drained = 0u;
        while (app_intercom_ws_rx_pop(s_ws_rx_packet_buf, &ws_packet_len) == 0) {
            app_intercom_handle_udp_packet(s_ws_rx_packet_buf, ws_packet_len);
            ws_drained = 1u;
        }

        if (s_ws_connected != 0) {
            if (ws_drained == 0u && s_rx_jitter_playing == 0u) {
                osal_delay_ms(5u);
            }
            app_intercom_jitter_play_tick();
            app_intercom_rx_stop_if_idle();
            continue;
        }

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
 *    b. 按 1 帧为一个 20ms PCM 包
 *    c. app_intercom_build_packet(packet, AUDIO, pcm, 640)
 *    d. service_network_udp_send(packet, len) —— 发给服务器
 *    e. 服务器收到后原样转发给同频道所有其他客户端
 * 5. 用户松手 → s_ptt_active = 0 → 退出循环
 * 6. 发送 PTT_STOP 控制包
 * 7. 打印发送统计（成功/失败帧数）
 * 8. 释放音频会话锁 → notify_take 阻塞等待下次
 *
 * ## 性能约束
 * - 每包 20ms (320 samples × 16bit = 640 字节)，服务器按 PCM 原样转发
 * - service_audio_read 超时 30ms，确保最坏情况下也不丢帧
 * - PTT 任务优先级 6（高于 AI 的 5），保证实时性
 *
 * @param arg 未使用。
 */
static void app_intercom_ptt_task(void *arg)
{
    (void)arg;
    int16_t pcm[APP_BUSINESS_FRAME_SAMPLES];       /* 20ms PCM 帧缓冲 */
    int16_t tx_pcm[APP_INTERCOM_PACKET_SAMPLES];    /* PCM 发送包缓冲 */
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

        /* PTT 期间按固定 20ms PCM 包发送，不做编解码和重传。 */
        (void)app_intercom_send_control(APP_INTERCOM_PKT_PTT_START);
        uint16_t tx_samples = 0u;
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
                uint16_t offset = 0u;
                while (offset < (uint16_t)samples) {
                    uint16_t space = (uint16_t)(APP_INTERCOM_PACKET_SAMPLES - tx_samples);
                    uint16_t copy_samples = (uint16_t)samples - offset;
                    if (copy_samples > space) {
                        copy_samples = space;
                    }
                    memcpy(&tx_pcm[tx_samples],
                           &pcm[offset],
                           (size_t)copy_samples * sizeof(int16_t));
                    tx_samples = (uint16_t)(tx_samples + copy_samples);
                    offset = (uint16_t)(offset + copy_samples);

                    if (tx_samples < APP_INTERCOM_PACKET_SAMPLES) {
                        continue;
                    }

                    int send_ret = app_intercom_send_audio_packet(packet, tx_pcm, tx_samples);
                    if (send_ret == 0) {
                        send_ok++;
                    } else {
                        send_fail++;
                        last_send_ret = send_ret;
                    }
                    tx_samples = 0u;
                }
            } else {
                read_fail++;
                last_read_ret = samples;
            }
        }
        if (tx_samples > 0u) {
            int send_ret = app_intercom_send_audio_packet(packet, tx_pcm, tx_samples);
            if (send_ret == 0) {
                send_ok++;
            } else {
                send_fail++;
                last_send_ret = send_ret;
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
 * - biz_ptt（优先级 6, 栈 8192）—— PTT 发送
 * - biz_udp_rx（优先级 5, 栈 10240）—— UDP 接收
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

    int ret = 0;
    if (service_network_is_ready() == 1) {
        ret = service_network_udp_connect(APP_BUSINESS_SERVER_HOST, APP_BUSINESS_UDP_PORT);
        if (ret != 0) {
            APP_LOGW(TAG, "UDP 对讲通道初始化失败, ret=%d", ret);
        } else {
            s_udp_ready = 1;
        }
    } else {
        APP_LOGI(TAG, "UDP 对讲通道等待网络就绪后连接");
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

    ret = app_intercom_start_ws_downlink_task();
    if (ret != 0) {
        APP_LOGW(TAG, "WebSocket 下行任务暂未启动, ret=%d", ret);
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
    s_ws_force_reconnect = 1;
    s_ws_reset_rx = 1;
    app_intercom_ws_rx_clear();
    if (s_heartbeat_task != NULL) {
        (void)osal_task_notify_give(s_heartbeat_task);
    }
}
