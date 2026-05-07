/**
 * @file bsp_common.h
 * @brief BSP 层公共配置。
 *
 * 本文件集中放置 BSP 组件共用的编译期开关，便于不同外设驱动保持一致的配置入口。
 */
#ifndef BSP_COMMON_H
#define BSP_COMMON_H

#include "hal/gpio_types.h"
#include "osal_log.h"

/**
 * @brief BSP 调试模式开关。
 *
 * 定义为 1 时开启调试逻辑，定义为 0 时关闭调试逻辑。
 */
#define BSP_DEBUG 1  // 默认开启，发布时改为 0

#if BSP_DEBUG
/**
 * @brief BSP 信息日志宏。
 */
#define BSP_LOGI(tag, fmt, ...) OSAL_LOGI(tag, fmt, ##__VA_ARGS__)

/**
 * @brief BSP 警告日志宏。
 */
#define BSP_LOGW(tag, fmt, ...) OSAL_LOGW(tag, fmt, ##__VA_ARGS__)

/**
 * @brief BSP 错误日志宏。
 */
#define BSP_LOGE(tag, fmt, ...) OSAL_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define BSP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define BSP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define BSP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
#endif

/**
 * @brief ES7210/ES8311 共用音频 I2S 引脚定义。
 */
#define BSP_AUDIO_CODEC_ENABLE_IO GPIO_NUM_16 /**< 音频 codec 板使能脚，高电平使能。 */
#define BSP_AUDIO_MCLK_IO GPIO_NUM_38 /**< MCLK。 */
#define BSP_AUDIO_BCLK_IO GPIO_NUM_14 /**< SCLK/BCLK。 */
#define BSP_AUDIO_LRCK_IO GPIO_NUM_47 /**< LRCK/WS。 */
#define BSP_AUDIO_DOUT_IO GPIO_NUM_48 /**< ESP 输出到 ES8311 DSDIN。 */
#define BSP_AUDIO_DIN_IO  GPIO_NUM_21 /**< ES7210 SDOUT1 输入到 ESP。 */

/**
 * @brief ES7210/ES8311 音频引脚别名。
 */
#define BSP_ES7210_MCLK_IO BSP_AUDIO_MCLK_IO /**< ES7210 MCLK。 */
#define BSP_ES7210_BCLK_IO BSP_AUDIO_BCLK_IO /**< ES7210 SCLK/BCLK。 */
#define BSP_ES7210_WS_IO   BSP_AUDIO_LRCK_IO /**< ES7210 LRCK/WS。 */
#define BSP_ES7210_DIN_IO  BSP_AUDIO_DIN_IO  /**< ES7210 SDOUT 接入 ESP。 */
#define BSP_ES8311_MCLK_IO BSP_AUDIO_MCLK_IO /**< ES8311 MCLK。 */
#define BSP_ES8311_BCLK_IO BSP_AUDIO_BCLK_IO /**< ES8311 SCLK/BCLK。 */
#define BSP_ES8311_WS_IO   BSP_AUDIO_LRCK_IO /**< ES8311 LRCK/WS。 */
#define BSP_ES8311_DOUT_IO BSP_AUDIO_DOUT_IO /**< ES8311 DSDIN 由 ESP 输出。 */
#define BSP_ES8311_ASDOUT_IO GPIO_NUM_NC /**< ADC 输出未使用。 */

/**
 * @brief ST7789 LCD SPI 引脚定义。
 */
#define BSP_LCD_SCK_IO   GPIO_NUM_12 /**< SPI SCK。 */
#define BSP_LCD_MOSI_IO  GPIO_NUM_11 /**< SPI MOSI。 */
#define BSP_LCD_MISO_IO  GPIO_NUM_NC /**< 未使用。 */
#define BSP_LCD_DC_IO    GPIO_NUM_18 /**< 数据/命令选择。 */
#define BSP_LCD_CS_IO    GPIO_NUM_8  /**< SPI 片选。 */
#define BSP_LCD_RST_IO   GPIO_NUM_NC /**< 复位脚未接。 */
#define BSP_LCD_BL_IO    GPIO_NUM_NC /**< 背光脚未接。 */

/**
 * @brief FT6336/FT5x06 触摸引脚定义。
 */
#define BSP_LCD_TOUCH_RST_IO GPIO_NUM_NC /**< 复位脚未接。 */
#define BSP_LCD_TOUCH_INT_IO GPIO_NUM_19 /**< 触摸中断。 */

/**
 * @brief 项目 I2C 总线引脚定义。
 */
#define BSP_I2C_SDA_IO GPIO_NUM_1 /**< I2C SDA。 */
#define BSP_I2C_SCL_IO GPIO_NUM_2 /**< I2C SCL。 */

/**
 * @brief OV5640 摄像头并口和控制引脚定义。
 */
#define BSP_CAMERA_VSYNC_IO GPIO_NUM_4  /**< 场同步。 */
#define BSP_CAMERA_HREF_IO  GPIO_NUM_5  /**< 行同步。 */
#define BSP_CAMERA_PCLK_IO  GPIO_NUM_16 /**< 像素时钟。 */
#define BSP_CAMERA_D0_IO    GPIO_NUM_39 /**< 数据 D0。 */
#define BSP_CAMERA_D1_IO    GPIO_NUM_40 /**< 数据 D1。 */
#define BSP_CAMERA_D2_IO    GPIO_NUM_42 /**< 数据 D2。 */
#define BSP_CAMERA_D3_IO    GPIO_NUM_41 /**< 数据 D3。 */
#define BSP_CAMERA_D4_IO    GPIO_NUM_17 /**< 数据 D4。 */
#define BSP_CAMERA_D5_IO    GPIO_NUM_15 /**< 数据 D5。 */
#define BSP_CAMERA_D6_IO    GPIO_NUM_7  /**< 数据 D6。 */
#define BSP_CAMERA_D7_IO    GPIO_NUM_6  /**< 数据 D7。 */
#define BSP_CAMERA_PWDN_IO  GPIO_NUM_NC /**< 电源关断未接。 */
#define BSP_CAMERA_RESET_IO GPIO_NUM_NC /**< 复位脚未接。 */
#define BSP_CAMERA_XCLK_IO  GPIO_NUM_NC /**< XCLK 未接。 */

#endif
