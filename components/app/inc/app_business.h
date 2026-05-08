/**
 * @file app_business.h
 * @brief 第一版业务控制器入口。
 */
#ifndef APP_BUSINESS_H
#define APP_BUSINESS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 service 并启动业务后台任务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_business_start(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUSINESS_H */
