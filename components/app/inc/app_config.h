/**
 * @file app_config.h
 * @brief App 层公共配置、业务常量和日志宏。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "osal_log.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief App 调试模式开关。
 *
 * 定义为 1 时开启调试日志，定义为 0 时关闭调试日志。
 */
#define APP_DEBUG 1

#if APP_DEBUG
/** @brief App 信息日志宏。 */
#define APP_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)
/** @brief App 警告日志宏。 */
#define APP_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)
/** @brief App 错误日志宏。 */
#define APP_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
/** @brief App 信息日志空实现。 */
#define APP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
/** @brief App 警告日志空实现。 */
#define APP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
/** @brief App 错误日志空实现。 */
#define APP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

/** @brief 全局设备 ID，AI、相机和对讲业务都使用同一个身份。 */
#define APP_DEVICE_ID                   "walkie-01"
/** @brief 兼容旧业务命名，统一指向 APP_DEVICE_ID。 */
#define APP_BUSINESS_DEVICE_NAME        APP_DEVICE_ID
/** @brief 业务服务器地址，AI HTTP 和对讲 WebSocket 使用同一公网主机。 */
#define APP_BUSINESS_SERVER_HOST        "139.129.17.67"
/** @brief WebSocket 对讲端口，独立于 AI/相机 HTTP 18080。 */
#define APP_BUSINESS_WS_PORT            18081
/** @brief WebSocket 对讲下行路由，设备会追加 device query。 */
#define APP_BUSINESS_WS_ROUTE_INTERCOM  "/intercom/ws"
/** @brief 对讲下行固定使用 WebSocket。 */
#define APP_INTERCOM_USE_WS_DOWNLINK    1
/** @brief 对讲上行固定使用 WebSocket，不再回退 UDP。 */
#define APP_INTERCOM_USE_WS_UPLINK      1
/** @brief Opus 裸 payload 上行链路测试；后端仍只转发 WTK1 binary 包。 */
#define APP_INTERCOM_OPUS_UPLINK_TEST_ENABLE 1
/** @brief Opus 裸 payload 下行链路测试；设备在 jitter buffer 播放阶段解码。 */
#define APP_INTERCOM_OPUS_DOWNLINK_TEST_ENABLE 1
/** @brief 对讲 Opus 目标码率。 */
#define APP_INTERCOM_OPUS_BITRATE       20000
/**
 * @brief Opus 本地 PTT 回放测试开关。
 *
 * 1 = 不建立对讲 WebSocket；按住 PTT 时采集并编码到 PSRAM，松手后本地解码播放。
 * 0 = 恢复正常 WebSocket 对讲。
 */
#define APP_INTERCOM_OPUS_LOCAL_TEST_ENABLE 0
/** @brief Opus 本地测试单次最长录音时间。 */
#define APP_INTERCOM_OPUS_LOCAL_TEST_MAX_MS 20000u
/** @brief FastAPI 业务服务根地址，AI 和相机路由由各业务模块追加。 */
#define APP_BUSINESS_HTTP_BASE_URL      "http://139.129.17.67:18080"
/** @brief WiFi OTA 服务固定根地址。 */
#define APP_OTA_BASE_URL                 "http://139.129.17.67:18082"
/** @brief OTA 服务识别的硬件型号。 */
#define APP_OTA_HARDWARE                 "walkie-v1"
/** @brief OTA 仅使用稳定发布通道。 */
#define APP_OTA_CHANNEL                  "stable"
/** @brief AI 创建会话路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_START        "/ai/start"
/** @brief AI 上传请求 WAV 分片路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_UPLOAD       "/ai/upload"
/** @brief AI 上传完成路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_FINISH       "/ai/finish"
/** @brief AI 回复状态查询路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_RESULT_INFO  "/ai/result_info"
/** @brief AI 回复 WAV 分片拉取路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_RESULT_CHUNK "/ai/result_chunk"
/** @brief AI 问答取消路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_CANCEL       "/ai/cancel"
/** @brief AI 回复语音停止播放路由。 */
#define APP_BUSINESS_HTTP_ROUTE_AI_STOP_AUDIO   "/ai/stop_audio"
/** @brief 相机 JPEG 上传路由。 */
#define APP_BUSINESS_HTTP_ROUTE_CAMERA_UPLOAD   "/camera/upload"
/** @brief 相机 JPEG 上传和同步图像分析 HTTP 超时时间，单位 ms。 */
#define APP_CAMERA_UPLOAD_TIMEOUT_MS    90000u
/** @brief 相机 JPEG 上传连接失败后的重试次数。 */
#define APP_CAMERA_UPLOAD_RETRY_COUNT   2u
/** @brief 相机 JPEG 上传重试间隔，单位 ms。 */
#define APP_CAMERA_UPLOAD_RETRY_DELAY_MS 800u
/** @brief 相机 JPEG 上传响应临时缓冲大小。 */
#define APP_CAMERA_UPLOAD_RESP_BYTES    1024u
/** @brief 相机预览区域 X 坐标。 */
#define APP_CAMERA_PREVIEW_X            0
/** @brief 相机预览区域 Y 坐标。 */
#define APP_CAMERA_PREVIEW_Y            30
/** @brief 相机预览区域宽度。 */
#define APP_CAMERA_PREVIEW_W            240
/** @brief 相机预览区域高度。 */
#define APP_CAMERA_PREVIEW_H            240
/** @brief 相机预览帧间隔，约 15 FPS。 */
#define APP_CAMERA_PREVIEW_INTERVAL_MS  100u
/** @brief 相机正常连续预览模式。 */
#define APP_CAMERA_PREVIEW_TEST_NORMAL  0
/** @brief 相机单帧冻结测试模式，用于判断花屏是否来自连续刷新。 */
#define APP_CAMERA_PREVIEW_TEST_SINGLE  1
/** @brief 相机固定色块测试模式，用于判断 LCD 直刷链路是否稳定。 */
#define APP_CAMERA_PREVIEW_TEST_COLOR   2
/**
 * @brief 当前相机预览测试模式。
 *
 * 调试花屏时先使用 SINGLE；若单帧仍花，再改为 COLOR 测 LCD 直刷路径。
 * 验证完成后改回 NORMAL。
 */
#define APP_CAMERA_PREVIEW_TEST_MODE    APP_CAMERA_PREVIEW_TEST_NORMAL
/** @brief AI HTTP 单片请求等待响应的超时时间，单位 ms。 */
#define APP_AI_HTTP_CHUNK_TIMEOUT_MS    60000u
/** @brief AI 服务器处理等待总超时时间，单位 ms。 */
#define APP_AI_PROCESS_TIMEOUT_MS       300000u
/** @brief AI 结果轮询间隔，单位 ms。 */
#define APP_AI_RESULT_POLL_MS           1000u
/** @brief AI 回复语音是否默认自动播放；0 表示需要用户点击播放按钮。 */
#define AUTO_PLAY_REPLY_AUDIO           0
/** @brief AI 请求音频上传分片大小，减小单次 body 可提升热点弱网下的上传稳定性。 */
#define APP_AI_UPLOAD_CHUNK_BYTES       8192u
/** @brief AI 回复音频拉取分片大小，保持较大片以减少播放时 HTTP 往返造成的卡顿。 */
#define APP_AI_REPLY_CHUNK_BYTES        32768u
/** @brief 开机默认频道号。 */
#define APP_BUSINESS_DEFAULT_CHANNEL    1
/** @brief 业务统一 PCM 采样率，单位 Hz。 */
#define APP_BUSINESS_AUDIO_SAMPLE_RATE  16000u
/** @brief 业务统一 PCM 位宽。 */
#define APP_BUSINESS_AUDIO_BITS         16u
/** @brief 业务统一 PCM 声道数，固定单声道。 */
#define APP_BUSINESS_AUDIO_CHANNELS     1u
/** @brief 对讲单包 20ms PCM 样本数。 */
#define APP_BUSINESS_FRAME_SAMPLES      320u
/** @brief 对讲单包 PCM 字节数。 */
#define APP_BUSINESS_FRAME_BYTES        (APP_BUSINESS_FRAME_SAMPLES * sizeof(int16_t))
/** @brief AI 单次录音最长时长，单位 ms。 */
#define APP_BUSINESS_AI_MAX_MS          60000u
/** @brief AI 回复音频最长时长，单位 ms。 */
#define APP_BUSINESS_AI_REPLY_MAX_MS    120000u
/** @brief AI 单次录音最大样本数。 */
#define APP_BUSINESS_AI_MAX_SAMPLES     ((APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AI_MAX_MS) / 1000u)
/** @brief AI 回复最大样本数。 */
#define APP_BUSINESS_AI_REPLY_MAX_SAMPLES \
    ((APP_BUSINESS_AUDIO_SAMPLE_RATE * APP_BUSINESS_AI_REPLY_MAX_MS) / 1000u)
/** @brief 标准 PCM WAV 文件头长度。 */
#define APP_BUSINESS_WAV_HEADER_LEN     44u
/** @brief AI 上传请求 WAV 最大字节数。 */
#define APP_BUSINESS_AI_REQUEST_WAV_MAX_BYTES \
    (APP_BUSINESS_WAV_HEADER_LEN + (APP_BUSINESS_AI_MAX_SAMPLES * sizeof(int16_t)))
/** @brief AI 回复 WAV 最大字节数。 */
#define APP_BUSINESS_AI_REPLY_WAV_MAX_BYTES \
    (APP_BUSINESS_WAV_HEADER_LEN + (APP_BUSINESS_AI_REPLY_MAX_SAMPLES * sizeof(int16_t)))
/** @brief AI 请求 WAV 缓冲区字节数；回复分片边播边丢弃，不再按回复最大值缓存。 */
#define APP_BUSINESS_AI_WAV_BUF_BYTES   APP_BUSINESS_AI_REQUEST_WAV_MAX_BYTES

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
