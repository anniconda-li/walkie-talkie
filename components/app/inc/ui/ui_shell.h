/**
 * @file ui_shell.h
 * @brief 主界面壳层接口
 */

#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdint.h>

/**
 * @brief 应用枚举
 *
 * 主界面壳层通过该枚举识别当前选中的应用。
 */
typedef enum {
    UI_APP_ID_INTERCOM = 0, /**< 对讲应用 */
    UI_APP_ID_CAMERA,       /**< 相机应用 */
    UI_APP_ID_AI,           /**< AI 应用 */
    UI_APP_ID_SETTINGS,     /**< 设置应用 */
    UI_APP_ID_COUNT         /**< 应用数量 */
} ui_app_id_t;

/**
 * @brief 初始化主界面壳层并默认显示 intercom
 *
 * 该接口会创建背景、状态栏、菜单栏、菜单开关以及应用内容挂载区，
 * 然后默认加载 intercom 应用内容。
 */
void ui_shell_init(void);

/**
 * @brief 刷新壳层语言相关文本
 *
 * settings 页面切换语言后调用。当前只刷新状态栏应用名；
 * 其他应用页面在重新进入时会按当前语言重建文本。
 */
void ui_shell_refresh_language(void);

/**
 * @brief 设置状态栏电量显示
 *
 * @param percent 电量百分比，范围 0-100。大于 100 会按 100 显示。
 *
 * Windows 模拟器可直接调用该接口测试 UI；ESP32-S3 移植时，
 * 将电池 ADC/电源管理芯片换算出的百分比传入这里即可。
 */
void ui_shell_set_battery_level(uint8_t percent);

/**
 * @brief 设置状态栏蜂窝信号强度
 *
 * @param level 信号档位，范围 0-4。0 表示无信号，4 表示满格。
 *
 * ML307C 4G 移植建议：
 *   业务层通过 AT+CSQ、AT+CPSI 或厂商网络状态命令获取信号质量，
 *   再映射成 0-4 档调用该接口。UI 层不直接发送 AT 命令。
 */
void ui_shell_set_signal_level(uint8_t level);

#endif /* UI_SHELL_H */
