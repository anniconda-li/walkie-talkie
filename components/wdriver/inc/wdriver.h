/**
 * @file wdriver.h
 * @brief WDRIVER 板级基础资源统一初始化接口。
 *
 * 本文件提供板级基础总线和底层资源的统一初始化入口。LCD、camera、LVGL、
 * 网络等重外设仍由业务层按需初始化，避免启动阶段强行占用大量资源。
 */
#ifndef WDRIVER_H
#define WDRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WDRIVER 基础资源。
 *
 * 当前会初始化 I2C、SPI、UART、I2S 和电池检测底层资源。
 *
 * @return 成功返回 0；失败返回负值。
 */
int wdriver_init(void);

/**
 * @brief 释放 WDRIVER 基础资源。
 *
 * @return 成功返回 0；失败返回负值。
 * @note UART 当前没有 deinit 接口，因此本函数暂不释放 UART。
 */
int wdriver_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* WDRIVER_H */
