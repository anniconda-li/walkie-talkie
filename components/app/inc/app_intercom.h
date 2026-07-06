/**
 * @file app_intercom.h
 * @brief UDP 实时对讲业务。
 *
 * 对上接收 UI 的频道和 PTT 操作；对下通过 service_audio 读取/播放 PCM，
 * 通过 service_network 发送/接收 UDP 数据。协议封装和心跳保活在模块内部完成。
 */
#ifndef APP_INTERCOM_H
#define APP_INTERCOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 UDP 对讲后台任务。
 *
 * 会创建心跳、UDP 接收和 PTT 发送任务，并尝试建立默认 UDP 通道。
 * 若首次 UDP 连接失败，心跳任务会在网络恢复后继续重试。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_intercom_start(void);

/**
 * @brief 设置当前对讲频道。
 *
 * 更新本地频道后会发送频道控制包给服务器，服务器据此维护设备所在频道。
 *
 * @param[in] channel 频道号；小于等于 0 时回退到默认频道。
 */
void app_intercom_set_channel(int32_t channel);

/**
 * @brief 开始 PTT 对讲发送。
 *
 * 该接口通常由 UI 长按事件调用。内部会唤醒 PTT 任务，由任务循环读取
 * 20ms PCM 帧并发送 UDP 音频包。
 *
 * @param[in] channel UI 当前频道号；小于等于 0 时沿用当前频道。
 */
void app_intercom_ptt_start(int32_t channel);

/**
 * @brief 停止 PTT 对讲发送。
 *
 * 该接口通常由 UI 松开事件调用。内部只清除 PTT 活动标志，PTT 任务会在
 * 当前帧发送结束后退出循环并发送 PTT_STOP。
 */
void app_intercom_ptt_stop(void);

/**
 * @brief 通知对讲模块网络后端已切换，需要重建 UDP 通道。
 */
void app_intercom_network_changed(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTERCOM_H */
