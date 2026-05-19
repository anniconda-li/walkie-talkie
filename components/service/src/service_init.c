/**
 * @file service_init.c
 * @brief Driver 与 Service 初始化装配实现。
 *
 * 本文件是 service 与具体 driver 的唯一装配点。service_xxx.c 不包含
 * driver 头文件，只知道自己定义的 ops；这里根据 service_config.h 中的
 * 选择宏，把已初始化 driver 暴露的函数填入对应 service 的配置结构。
 */
#include "service_init.h"

#include "driver_battery.h"
#include "driver_camera.h"
#include "driver_es8311.h"
#include "driver_es7210.h"
#include "driver_inmp441.h"
#include "driver_lcd.h"
#include "driver_max98357a.h"
#include "driver_ml307c.h"
#include "driver_wifi.h"
#include "service_audio.h"
#include "service_battery.h"
#include "service_camera.h"
#include "service_network.h"
#include "service_screen.h"

#include <stddef.h>

static const char *TAG = "service_init";

/**
 * @brief 将 LCD driver 的坐标接口适配为 service_screen 的 x/y/w/h 接口。
 *
 * App 只看到 service_screen_draw_rgb565(x, y, w, h)，不直接依赖 driver_lcd。
 */
static int service_init_screen_draw_rgb565(int x, int y, int w, int h, const void *data)
{
    return driver_lcd_draw_bitmap(x, y, x + w, y + h, data);
}

/**
 * @brief 获取 LCD panel IO 不透明句柄。
 *
 * service_screen.h 不暴露 ESP LCD 类型，装配层把具体 driver 句柄转成 void *，
 * service_screen.c 内部再按平台实现转换回 ESP 类型。
 */
static void *service_init_screen_get_panel_io(void)
{
    return (void *)driver_lcd_get_panel_io_handle();
}

/**
 * @brief 获取 LCD panel 不透明句柄。
 */
static void *service_init_screen_get_panel(void)
{
    return (void *)driver_lcd_get_panel_handle();
}

/**
 * @brief 获取触摸控制器不透明句柄。
 */
static void *service_init_screen_get_touch(void)
{
    return (void *)driver_lcd_get_touch_handle();
}

/**
 * @brief 将 esp32-camera 的帧格式映射到 service_camera 的通用格式。
 */
static service_camera_format_t service_init_camera_map_format(pixformat_t format)
{
    if (format == PIXFORMAT_RGB565) {
        return SERVICE_CAMERA_FORMAT_RGB565;
    }
    if (format == PIXFORMAT_JPEG) {
        return SERVICE_CAMERA_FORMAT_JPEG;
    }

    return SERVICE_CAMERA_FORMAT_UNKNOWN;
}

/**
 * @brief 获取一帧 camera driver 图像并包装成 service_camera_frame_t。
 *
 * camera_fb_t 只在装配层出现，service_camera 和 app 都不需要包含 esp_camera.h。
 * opaque 保存原始 fb 指针，后续 return wrapper 用它归还底层帧缓存。
 */
static int service_init_camera_get_frame(service_camera_frame_t *frame)
{
    if (frame == NULL) {
        return -1;
    }

    camera_fb_t *fb = driver_camera_get_frame();
    if (fb == NULL) {
        return -2;
    }

    frame->data = fb->buf;
    frame->len = fb->len;
    frame->width = (uint16_t)fb->width;
    frame->height = (uint16_t)fb->height;
    frame->format = service_init_camera_map_format(fb->format);
    frame->opaque = fb;
    if (fb->format == PIXFORMAT_JPEG) {
        const uint8_t head0 = fb->len > 0u && fb->buf != NULL ? fb->buf[0] : 0u;
        const uint8_t head1 = fb->len > 1u && fb->buf != NULL ? fb->buf[1] : 0u;
        const uint8_t tail0 = fb->len > 1u && fb->buf != NULL ? fb->buf[fb->len - 2u] : 0u;
        const uint8_t tail1 = fb->len > 0u && fb->buf != NULL ? fb->buf[fb->len - 1u] : 0u;
        SERVICE_LOGI(TAG,
                     "camera fb JPEG, w=%u, h=%u, len=%u, head=%02X%02X, tail=%02X%02X",
                     (unsigned int)fb->width,
                     (unsigned int)fb->height,
                     (unsigned int)fb->len,
                     (unsigned int)head0,
                     (unsigned int)head1,
                     (unsigned int)tail0,
                     (unsigned int)tail1);
    }
    return 0;
}

/**
 * @brief 归还 service_camera_frame_t 中保存的底层 camera frame。
 */
static void service_init_camera_return_frame(void *opaque)
{
    if (opaque != NULL) {
        driver_camera_return_frame((camera_fb_t *)opaque);
    }
}

#if SERVICE_INIT_NETWORK == SERVICE_INIT_NETWORK_ML307C
/**
 * @brief 将 ML307C driver 状态转换为通用 service_network_status_t。
 *
 * ML307C 的状态字段来自 AT、SIM、CEREG、ISLINK、CSQ 等蜂窝链路概念。
 * service 层只关心通用字段，因此在装配层完成一次字段映射。
 *
 * @param[out] status 通用网络状态输出。
 * @return 成功返回 0；失败返回负值。
 */
static int service_init_network_ml307c_get_status(service_network_status_t *status)
{
    if (status == NULL) {
        return -1;
    }

    driver_ml307c_status_t driver_status;
    int ret = driver_ml307c_get_status(&driver_status);
    if (ret != 0) {
        return ret;
    }

    status->rssi = driver_status.rssi;
    status->reg_state = driver_status.reg_state;
    status->link_state = driver_status.link_state;
    status->sim_ready = driver_status.sim_ready;
    status->at_ready = driver_status.at_ready;
    return 0;
}

#endif

#if SERVICE_INIT_NETWORK == SERVICE_INIT_NETWORK_WIFI
/**
 * @brief 将 WiFi driver 状态转换为通用 service_network_status_t。
 *
 * WiFi 没有 SIM/AT/CEREG 概念，因此将 sim_ready/at_ready 固定映射为 1，
 * link_ready 同时作为 reg_state 和 link_state 使用，保证 UI 和业务层可复用
 * 同一套网络状态判断。
 *
 * @param[out] status 通用网络状态输出。
 * @return 成功返回 0；失败返回负值。
 */
static int service_init_network_wifi_get_status(service_network_status_t *status)
{
    if (status == NULL) {
        return -1;
    }

    driver_wifi_status_t wifi_status;
    int ret = driver_wifi_get_status(&wifi_status);
    if (ret != 0) {
        return ret;
    }

    status->rssi = wifi_status.rssi;
    status->reg_state = wifi_status.link_ready ? 1 : 0;
    status->link_state = wifi_status.link_ready;
    status->sim_ready = 1;
    status->at_ready = 1;
    return 0;
}

#endif

int service_init_network(void)
{
    /*
     * 局部 cfg 是安全的：service_network_init() 会复制 ops 到自身静态变量。
     * 初始化返回后 cfg 生命周期结束，不影响后续 service 调用。
     */
    service_network_config_t network_cfg = {
        .ops = {
#if SERVICE_INIT_NETWORK == SERVICE_INIT_NETWORK_WIFI
            .is_initialized = driver_wifi_is_initialized,
            .get_status = service_init_network_wifi_get_status,
            .is_ready = driver_wifi_is_ready,
            .tcp_connect = driver_wifi_tcp_connect,
            .tcp_send = driver_wifi_tcp_send,
            .tcp_close = driver_wifi_tcp_close,
            .udp_connect = driver_wifi_udp_connect,
            .udp_send = driver_wifi_udp_send,
            .read_downlink = driver_wifi_read_downlink,
            .http_post = driver_wifi_http_post,
            .http_post_wav = driver_wifi_http_post_wav,
#else
            .is_initialized = driver_ml307c_is_initialized,
            .get_status = service_init_network_ml307c_get_status,
            .is_ready = driver_ml307c_is_ready,
            .tcp_connect = driver_ml307c_tcp_connect,
            .tcp_send = driver_ml307c_tcp_send,
            .tcp_close = driver_ml307c_tcp_close,
            .udp_connect = driver_ml307c_udp_connect,
            .udp_send = driver_ml307c_udp_send,
            .read_downlink = driver_ml307c_read_downlink,
            .http_post = driver_ml307c_http_post,
            .http_post_wav = driver_ml307c_http_post_wav,
#endif
        },
    };
    int ret = service_network_init(&network_cfg);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init_network_recover(void)
{
    int ret = driver_network_init();
    if (ret != 0) {
        SERVICE_LOGW(TAG, "网络 driver 恢复失败, ret=%d", ret);
        return ret;
    }

    return service_init_network();
}

int service_init_audio(void)
{
    /*
     * 音频 service 只需要“读 PCM”和“播 PCM/音量/静音”能力。
     * 具体装配 ES7210+ES8311 还是 INMP441+MAX98357A 由 SERVICE_INIT_AUDIO 决定。
     */
    service_audio_config_t audio_cfg = {
        .capture_ops = {
#if SERVICE_INIT_AUDIO == SERVICE_INIT_AUDIO_I2S
            .is_initialized = driver_inmp441_is_initialized,
            .read_pcm = driver_inmp441_read_pcm,
#else
            .is_initialized = driver_es7210_is_initialized,
            .read_pcm = driver_es7210_read_pcm,
#endif
        },
        .playback_ops = {
#if SERVICE_INIT_AUDIO == SERVICE_INIT_AUDIO_I2S
            .is_initialized = driver_max98357a_is_initialized,
            .play_pcm = driver_max98357a_play_pcm,
            .set_volume = driver_max98357a_set_volume,
            .set_mute = driver_max98357a_set_mute,
#else
            .is_initialized = driver_es8311_is_initialized,
            .play_pcm = driver_es8311_play_pcm,
            .set_volume = driver_es8311_set_volume,
            .set_mute = driver_es8311_set_mute,
#endif
        },
        .volume = 80u,
        .passthrough_gain = 1u,
    };
    int ret = service_audio_init(&audio_cfg);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "音频服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init_camera(void)
{
    service_camera_config_t camera_cfg = {
        .ops = {
            .is_initialized = driver_camera_is_initialized,
            .set_rgb565_mode = driver_camera_set_rgb565_mode,
            .set_jpeg_mode = driver_camera_set_jpeg_mode,
            .get_frame = service_init_camera_get_frame,
            .return_frame = service_init_camera_return_frame,
        },
    };

    int ret = service_camera_init(&camera_cfg);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "摄像头服务初始化失败或未启用, ret=%d", ret);
    }

    return ret;
}

int service_init_screen(void)
{
    service_screen_config_t screen_cfg = {
        .device_ops = {
            .is_initialized = driver_lcd_is_initialized,
            .get_panel_io = service_init_screen_get_panel_io,
            .get_panel = service_init_screen_get_panel,
            .get_touch = service_init_screen_get_touch,
            .display_on = driver_lcd_display_on,
            .draw_rgb565 = service_init_screen_draw_rgb565,
            .hres = driver_lcd_H_RES,
            .vres = driver_lcd_V_RES,
            .swap_xy = driver_lcd_SWAP_XY,
            .mirror_x = driver_lcd_MIRROR_X,
            .mirror_y = driver_lcd_MIRROR_Y,
        },
    };

    int ret = service_screen_init(&screen_cfg);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init_battery(void)
{
    service_battery_config_t battery_cfg = {
        .sample_ops = {
            .is_initialized = driver_battery_is_initialized,
            .read_voltage_mv = driver_battery_read_voltage_mv,
        },
    };
    int ret = service_battery_init(&battery_cfg);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "电池服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init(void)
{
    /*
     * 总装配顺序与 app 依赖一致：
     * screen 先初始化，app_business_start() 才能创建 UI；
     * battery/status/audio/network 之后由 app 后台任务持续使用。
     */
    int ret = service_init_screen();
    if (ret != 0) {
        return ret;
    }

    ret = service_init_battery();
    if (ret != 0) {
        return ret;
    }

    ret = service_init_audio();
    if (ret != 0) {
        return ret;
    }

    ret = service_init_network();
    if (ret != 0) {
        return ret;
    }

#if DRIVER_INIT_ENABLE_CAMERA
    /*
     * 摄像头当前仍是可选业务：只有 driver 层明确启用 camera 初始化时，
     * 才在总 service_init() 中装配 camera service。这样未接摄像头时不会
     * 影响对讲、AI、UI 等主业务启动。
     */
    (void)service_init_camera();
#endif
    return 0;
}
