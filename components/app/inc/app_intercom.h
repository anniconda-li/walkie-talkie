/**
 * @file app_intercom.h
 * @brief UDP 实时对讲业务。
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
 * 会创建心跳、UDP 接收和 PTT 发送任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_intercom_start(void);

/**
 * @brief 设置当前对讲频道。
 *
 * @param[in] channel 频道号；小于等于 0 时回退到默认频道。
 */
void app_intercom_set_channel(int32_t channel);

/**
 * @brief 开始 PTT 对讲发送。
 *
 * @param[in] channel UI 当前频道号；小于等于 0 时沿用当前频道。
 */
void app_intercom_ptt_start(int32_t channel);

/**
 * @brief 停止 PTT 对讲发送。
 */
void app_intercom_ptt_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTERCOM_H */
