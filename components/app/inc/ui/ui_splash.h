
/**
 * @file ui_splash.h
 * @brief 启动画面接口
 */

#ifndef UI_SPLASH_H
#define UI_SPLASH_H

/**
 * @brief 显示启动画面动画。
 *
 * 启动画面会显示当前开机阶段状态，由启动编排持续刷新。
 */
void splash_screen(void);

/**
 * @brief 从 app_boot_status 刷新启动状态列表。
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
