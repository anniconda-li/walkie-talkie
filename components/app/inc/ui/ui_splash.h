
/**
 * @file ui_splash.h
 * @brief 启动画面接口
 */

#ifndef UI_SPLASH_H
#define UI_SPLASH_H

/**
 * @brief 显示启动画面。
 *
 * 启动画面会全屏显示品牌 logo，直到启动编排完成后切入主界面。
 */
void splash_screen(void);

/**
 * @brief 刷新启动状态。
 *
 * 当前品牌启动页不展示内部初始化阶段，本接口保留给启动编排调用。
 */
void ui_splash_refresh_status(void);

/**
 * @brief 显示致命启动错误。
 *
 * @param[in] stage 失败阶段名称。
 * @param[in] code 错误码。
 */
void ui_splash_show_error(const char *stage, int code);

/**
 * @brief 结束启动页并进入主界面。
 */
void ui_splash_finish(void);

#endif /* UI_SPLASH_H */
