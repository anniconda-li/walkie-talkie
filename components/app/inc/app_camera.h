/**
 * @file app_camera.h
 * @brief 相机业务模块接口。
 *
 * 本模块组合 service_camera、service_screen 和 service_network 完成相机页业务：
 * 实时预览、拍照定格、JPEG 暂存和 HTTP 上传。UI 层只通过 app_business 注册
 * 的回调触发这些接口，不直接接触摄像头、LCD 或网络细节。
 */
#ifndef APP_CAMERA_H
#define APP_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动相机业务后台任务。
 *
 * 如果当前未启用 camera service，本函数只记录日志并返回成功，避免未接摄像头
 * 时影响对讲和 AI 主业务。
 *
 * @return 成功返回 0；失败返回负值。
 */
int app_camera_start(void);

/**
 * @brief 通知相机页进入，开始 RGB565 预览。
 */
void app_camera_enter(void);

/**
 * @brief 通知相机页退出，停止预览并释放暂存 JPEG。
 */
void app_camera_exit(void);

/**
 * @brief 请求拍照。
 *
 * 实际 JPEG 获取在相机后台任务执行，UI 回调只负责发起请求。
 */
void app_camera_capture(void);

/**
 * @brief 请求上传当前暂存 JPEG。
 */
void app_camera_upload(void);

/**
 * @brief 请求重拍。
 *
 * 会释放当前暂存 JPEG，并恢复 RGB565 预览。
 */
void app_camera_retake(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAMERA_H */
