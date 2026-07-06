/**
 * @file service_init.c
 * @brief Driver 与 Service 初始化装配实现。
 *
 * 本文件是 service 与具体 driver 的唯一装配点。service_xxx.c 不包含
 * driver 头文件，只知道自己定义的 ops；这里根据 service_config.h 中的
 * 选择宏，把已初始化 driver 暴露的函数填入对应 service 的配置结构。
 */
#include "service_init.h"

#include "d_battery.h"
#include "d_camera.h"
#include "d_es8311.h"
#include "d_es7210.h"
#include "d_init.h"
#include "d_lcd.h"
#include "d_pca9557.h"
#include "d_wifi.h"
#include "service_audio.h"
#include "service_battery.h"
#include "service_buttons.h"
#include "service_camera.h"
#include "service_network.h"
#include "service_screen.h"

#include <stddef.h>

/** @brief service 装配层日志标签。 */
static const char *TAG = "service_init";

/**
 * @brief 将 LCD driver 的坐标接口适配为 service_screen 的 x/y/w/h 接口。
 *
 * App 只看到 service_screen_draw_rgb565(x, y, w, h)，不直接依赖 d_lcd。
 */
static int service_init_screen_draw_rgb565(int x, int y, int w, int h, const void *data)
{
    return d_lcd_draw_bitmap(x, y, x + w, y + h, data);
}

/**
 * @brief 获取 LCD panel IO 不透明句柄。
 *
 * service_screen.h 不暴露 ESP LCD 类型，装配层把具体 driver 句柄转成 void *，
 * service_screen.c 内部再按平台实现转换回 ESP 类型。
 */
static void *service_init_screen_get_panel_io(void)
{
    return (void *)d_lcd_get_panel_io_handle();
}

/**
 * @brief 获取 LCD panel 不透明句柄。
 */
static void *service_init_screen_get_panel(void)
{
    return (void *)d_lcd_get_panel_handle();
}

/**
 * @brief 获取触摸控制器不透明句柄。
 */
static void *service_init_screen_get_touch(void)
{
    return (void *)d_lcd_get_touch_handle();
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

    camera_fb_t *fb = d_camera_get_frame();
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
        d_camera_return_frame((camera_fb_t *)opaque);
    }
}

/**
 * @brief 将 WiFi driver 状态转换为通用 service_network_status_t。
 *
 * @param[out] status 通用网络状态输出。
 * @return 成功返回 0；失败返回负值。
 */
static int service_init_network_wifi_get_status(service_network_status_t *status)
{
    if (status == NULL) {
        return -1;
    }

    d_wifi_status_t wifi_status;
    int ret = d_wifi_get_status(&wifi_status);
    if (ret != 0) {
        return ret;
    }

    status->rssi = wifi_status.rssi;
    status->link_ready = wifi_status.link_ready;
    return 0;
}

static service_network_backend_t s_network_backend = SERVICE_NETWORK_BACKEND_WIFI;

static int service_init_buttons_read_pca_pin(pca9557_pin_t pin,
                                             service_buttons_level_t *level)
{
    if (level == NULL) {
        return -1;
    }

    pca9557_level_t pca_level = PCA9557_LEVEL_HIGH;
    int ret = d_pca9557_get_pin_level(pin, &pca_level);
    if (ret != 0) {
        return ret;
    }

    *level = (pca_level == PCA9557_LEVEL_HIGH) ?
             SERVICE_BUTTONS_LEVEL_HIGH :
             SERVICE_BUTTONS_LEVEL_LOW;
    return 0;
}

static int service_init_buttons_read_volume_up(service_buttons_level_t *level)
{
    return service_init_buttons_read_pca_pin(PCA9557_PIN_1, level);
}

static int service_init_buttons_read_volume_down(service_buttons_level_t *level)
{
    return service_init_buttons_read_pca_pin(PCA9557_PIN_2, level);
}

int service_init_network(void)
{
    return service_init_network_for(s_network_backend);
}

int service_init_network_for(service_network_backend_t backend)
{
    /*
     * 局部 ops 是安全的：service_network_init() 会复制 ops 到自身静态变量。
     * 初始化返回后 ops 生命周期结束，不影响后续 service 调用。
     */
    service_network_ops_t network_ops = {0};

    (void)backend;
    network_ops.is_initialized = d_wifi_is_initialized;
    network_ops.get_status = service_init_network_wifi_get_status;
    network_ops.is_ready = d_wifi_is_ready;
    network_ops.tcp_connect = d_wifi_tcp_connect;
    network_ops.tcp_send = d_wifi_tcp_send;
    network_ops.tcp_close = d_wifi_tcp_close;
    network_ops.udp_connect = d_wifi_udp_connect;
    network_ops.udp_send = d_wifi_udp_send;
    network_ops.read_downlink = d_wifi_read_downlink;
    network_ops.http_post = d_wifi_http_post;

    (void)service_network_deinit();
    int ret = service_network_init(&network_ops);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "网络服务初始化失败, ret=%d", ret);
        return ret;
    }

    s_network_backend = SERVICE_NETWORK_BACKEND_WIFI;
    SERVICE_LOGI(TAG, "网络服务已切换到 WiFi");
    return 0;
}

service_network_backend_t service_network_get_backend(void)
{
    return s_network_backend;
}

int service_init_network_recover(void)
{
    return service_init_network();
}

int service_init_audio(void)
{
    /*
     * 音频 service 只需要“读 PCM”和“播 PCM/音量/静音”能力。
     * 当前硬件固定装配 ES7210 + ES8311。
     */
    service_audio_config_t audio_cfg = {
        .capture_ops = {
            .is_initialized = d_es7210_is_initialized,
            .read_pcm = d_es7210_read_pcm,
        },
        .playback_ops = {
            .is_initialized = d_es8311_is_initialized,
            .start_playback = d_es8311_start_playback,
            .stop_playback = d_es8311_stop_playback,
            .play_pcm = d_es8311_play_pcm,
            .set_volume = d_es8311_set_volume,
            .set_mute = d_es8311_set_mute,
        },
        .volume = 80u,
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
            .init = d_camera_init,
            .deinit = d_camera_deinit,
            .is_initialized = d_camera_is_initialized,
            .set_rgb565_mode = d_camera_set_rgb565_mode,
            .set_jpeg_mode = d_camera_set_jpeg_mode,
            .get_frame = service_init_camera_get_frame,
            .return_frame = service_init_camera_return_frame,
        },
    };

    int ret = service_camera_init(&camera_cfg);
    if (ret != 0) {
        SERVICE_LOGW(TAG, "摄像头服务初始化失败或未启用, ret=%d", ret);
    } else {
        (void)service_camera_power_off();
        SERVICE_LOGI(TAG, "摄像头服务已绑定，空闲时保持下电");
    }

    return ret;
}

int service_init_screen(void)
{
    service_screen_device_ops_t screen_ops = {
        .is_initialized = d_lcd_is_initialized,
        .get_panel_io = service_init_screen_get_panel_io,
        .get_panel = service_init_screen_get_panel,
        .get_touch = service_init_screen_get_touch,
        .display_on = d_lcd_display_on,
        .draw_rgb565 = service_init_screen_draw_rgb565,
        .hres = d_lcd_H_RES,
        .vres = d_lcd_V_RES,
        .swap_xy = d_lcd_SWAP_XY,
        .mirror_x = d_lcd_MIRROR_X,
        .mirror_y = d_lcd_MIRROR_Y,
    };

    int ret = service_screen_init(&screen_ops);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "屏幕服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init_battery(void)
{
    service_battery_sample_ops_t battery_ops = {
        .is_initialized = d_battery_is_initialized,
        .read_voltage_mv = d_battery_read_voltage_mv,
    };
    int ret = service_battery_init(&battery_ops);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "电池服务初始化失败, ret=%d", ret);
    }

    return ret;
}

int service_init_buttons(void)
{
    service_buttons_config_t buttons_cfg = {
        .read_volume_up = service_init_buttons_read_volume_up,
        .read_volume_down = service_init_buttons_read_volume_down,
        .active_level = SERVICE_BUTTONS_LEVEL_LOW,
    };

    int ret = service_buttons_init(&buttons_cfg);
    if (ret != 0) {
        SERVICE_LOGE(TAG, "实体按键服务初始化失败, ret=%d", ret);
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

    ret = service_init_buttons();
    if (ret != 0) {
        return ret;
    }

    ret = service_init_audio();
    if (ret != 0) {
        return ret;
    }

#if D_INIT_ENABLE_CAMERA
    /*
     * 摄像头当前仍是可选业务：只有 driver 层明确启用 camera 初始化时，
     * 才在总 service_init() 中装配 camera service。这样未接摄像头时不会
     * 影响对讲、AI、UI 等主业务启动。
     */
    (void)service_init_camera();
#endif
    return 0;
}
