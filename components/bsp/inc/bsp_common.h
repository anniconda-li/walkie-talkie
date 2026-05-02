/**
 * @file bsp_common.h
 * @brief BSP 层公共配置。
 *
 * 本文件集中放置 BSP 组件共用的编译期开关，便于不同外设驱动保持一致的配置入口。
 */
#ifndef BSP_COMMON_H
#define BSP_COMMON_H

/**
 * @brief BSP 调试模式开关。
 *
 * 定义为 1 时开启调试逻辑，定义为 0 时关闭调试逻辑。
 */
#define BSP_DEBUG 1  // 默认开启，发布时改为 0



#endif
