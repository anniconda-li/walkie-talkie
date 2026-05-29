/**
 * @file wdriver_spi.h
 * @brief WDRIVER SPI 主机总线驱动接口。
 *
 * 本文件提供项目默认 SPI 总线的初始化、释放和 host 获取接口。SPI 总线由 WDRIVER SPI
 * 统一管理，LCD、后续 SD 卡等设备只复用该总线，不重复初始化 SPI host。
 */
#ifndef WDRIVER_SPI_H
#define WDRIVER_SPI_H

#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化项目默认 SPI 总线。
 *
 * @return 成功返回 0；失败返回负值。
 */
int wdriver_spi_init(void);

/**
 * @brief 释放项目默认 SPI 总线。
 *
 * @return 成功返回 0；失败返回负值。
 * @note 当前 LCD 不主动释放 SPI 总线，避免未来多个 SPI 设备共享总线时生命周期冲突。
 */
int wdriver_spi_deinit(void);

/**
 * @brief 获取项目默认 SPI host。
 *
 * @return SPI host 编号。
 */
spi_host_device_t wdriver_spi_get_host(void);

#ifdef __cplusplus
}
#endif

#endif /* WDRIVER_SPI_H */
