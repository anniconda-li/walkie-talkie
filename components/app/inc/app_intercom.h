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

int app_intercom_start(void);
void app_intercom_set_channel(int32_t channel);
void app_intercom_ptt_start(int32_t channel);
void app_intercom_ptt_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTERCOM_H */
