/**
 * @file app_intercom_opus_test.h
 * @brief Opus 本地 PTT 录制回放测试接口。
 */
#ifndef APP_INTERCOM_OPUS_TEST_H
#define APP_INTERCOM_OPUS_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

int app_intercom_opus_test_start(void);
void app_intercom_opus_test_ptt_start(void);
void app_intercom_opus_test_ptt_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTERCOM_OPUS_TEST_H */
