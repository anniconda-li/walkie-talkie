/**
 * @file d_camera.c
 * @brief 基于 esp32-camera 组件的 WDRIVER 摄像头驱动实现。
 */
#include "d_camera.h"

#include "d_config.h"
#include "wdriver_i2c.h"
#include "driver/gpio.h"

#include <stdint.h>

/**
 * @brief 摄像头日志标签。
 */
static const char *TAG = "d_camera";

/**
 * @brief 摄像头 XCLK 频率。
 */
#define d_camera_XCLK_FREQ_HZ 20000000

/** @brief 本地预览使用的帧尺寸。 */
#define D_CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_240X240

/** @brief 拍照上传使用的 JPEG 帧尺寸，第一版使用 QVGA 兼顾兼容性和数据量。 */
#define D_CAMERA_CAPTURE_FRAME_SIZE FRAMESIZE_QVGA

/** @brief 摄像头 driver 是否已成功初始化。 */
static uint8_t s_camera_inited = 0u;

/** @brief 当前摄像头输出格式，用于避免重复切换 sensor 模式。 */
static pixformat_t s_camera_pixformat = PIXFORMAT_RGB565;

/** @brief 当前摄像头输出尺寸，用于避免重复切换 sensor 模式。 */
static framesize_t s_camera_framesize = D_CAMERA_PREVIEW_FRAME_SIZE;

/**
 * @brief 将底层驱动错误码转换为 WDRIVER 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int d_camera_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

static int d_camera_set_pwdn_level(int level)
{
    if (d_camera_PWDN_IO == GPIO_NUM_NC) {
        return 0;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << d_camera_PWDN_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    int ret = d_camera_err_to_int(gpio_config(&cfg));
    if (ret != 0) {
        D_LOGE(TAG, "摄像头 PWDN 引脚配置失败, io=%d, ret=%d", d_camera_PWDN_IO, ret);
        return ret;
    }

    ret = d_camera_err_to_int(gpio_set_level(d_camera_PWDN_IO, level != 0 ? 1 : 0));
    if (ret != 0) {
        D_LOGE(TAG, "摄像头 PWDN 设置失败, io=%d, level=%d, ret=%d",
               d_camera_PWDN_IO,
               level,
               ret);
    }
    return ret;
}

/**
 * @brief 调整 sensor 默认画面参数。
 *
 * 只在 RGB565 预览模式做轻量调校，改善默认画面偏白、偏灰的问题。不同
 * sensor 对取值范围和支持项的实现可能不同，因此这里按 best-effort 调用。
 */
static void d_camera_apply_preview_tuning(pixformat_t pixformat)
{
    if (pixformat != PIXFORMAT_RGB565) {
        return;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        D_LOGW(TAG, "摄像头预览参数调校跳过: sensor 不可用");
        return;
    }

    int ret = 0;
    if (sensor->set_special_effect != NULL) {
        ret = sensor->set_special_effect(sensor, 0);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头特效关闭失败, ret=%d", ret);
        }
    }
    if (sensor->set_whitebal != NULL) {
        ret = sensor->set_whitebal(sensor, 1);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头自动白平衡设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_awb_gain != NULL) {
        ret = sensor->set_awb_gain(sensor, 1);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头 AWB gain 设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_wb_mode != NULL) {
        ret = sensor->set_wb_mode(sensor, 0);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头白平衡模式设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_gain_ctrl != NULL) {
        ret = sensor->set_gain_ctrl(sensor, 1);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头自动增益设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_agc_gain != NULL) {
        ret = sensor->set_agc_gain(sensor, 0);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头增益档位设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_exposure_ctrl != NULL) {
        ret = sensor->set_exposure_ctrl(sensor, 1);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头自动曝光设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_aec2 != NULL) {
        ret = sensor->set_aec2(sensor, 1);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头 AEC2 设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_saturation != NULL) {
        ret = sensor->set_saturation(sensor, 2);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头饱和度设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_contrast != NULL) {
        ret = sensor->set_contrast(sensor, 2);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头对比度设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_brightness != NULL) {
        ret = sensor->set_brightness(sensor, -2);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头亮度设置失败, ret=%d", ret);
        }
    }
    if (sensor->set_ae_level != NULL) {
        ret = sensor->set_ae_level(sensor, -2);
        if (ret != 0) {
            D_LOGW(TAG, "摄像头自动曝光等级设置失败, ret=%d", ret);
        }
    }

    D_LOGI(TAG,
                "摄像头预览参数已调校: awb=on, agc=on, agc_gain=0, aec=on, ae_level=-2, brightness=-2, saturation=2, contrast=2");
}

/**
 * @brief 打印当前 esp-camera 自动识别到的传感器信息。
 *
 * 当前驱动不再绑定固定传感器型号，OV2640/OV5640 都交给 esp-camera
 * 自动探测。这里打印 PID 和当前格式，便于换模组后确认实际识别结果。
 */
static void d_camera_log_sensor_info(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        D_LOGW(TAG, "摄像头 sensor 信息不可用");
        return;
    }

    D_LOGI(TAG,
                "摄像头 sensor 已识别, pid=0x%04X, slv_addr=0x%02X, pixformat=%d, framesize=%d",
                (unsigned int)sensor->id.PID,
                (unsigned int)sensor->slv_addr,
                (int)sensor->pixformat,
                (int)sensor->status.framesize);
}

/**
 * @brief 按指定输出格式初始化 esp-camera。
 *
 * esp-camera 的 JPEG/RGB565 采样模式、DMA 接收长度和帧缓存处理是在
 * esp_camera_init() 内根据 camera_config_t.pixel_format 配好的。运行时只
 * 调 sensor->set_pixformat() 会让 sensor 状态变成 JPEG，但 HAL 仍按旧的
 * RGB/YUV 接收路径取帧，表现为 fb->format 是 JPEG、数据却不是 JPEG。
 * 因此本项目在 RGB565 预览和 JPEG 拍照之间切换时，采用 deinit/init 重建。
 */
static int d_camera_init_mode(pixformat_t pixformat, framesize_t framesize)
{
    if (wdriver_i2c_get_bus_handle() == NULL) {
        D_LOGE(TAG, "摄像头初始化失败: I2C 未初始化");
        return -1;
    }

    int ret = d_camera_set_pwdn_level(0);
    if (ret != 0) {
        return ret;
    }

    camera_config_t camera_config = {
        .pin_pwdn = d_camera_PWDN_IO,
        .pin_reset = d_camera_RESET_IO,
        .pin_xclk = d_camera_XCLK_IO,
        /*
         * 摄像头挂在 WDRIVER 已初始化的共享 I2C 总线上。按 esp-camera 的
         * camera_config_t 约定，SDA 传 -1 时会使用 sccb_i2c_port 指定的
         * 已配置 I2C bus，避免 camera 再次安装同一个 I2C driver。
         */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d7 = d_camera_D7_IO,
        .pin_d6 = d_camera_D6_IO,
        .pin_d5 = d_camera_D5_IO,
        .pin_d4 = d_camera_D4_IO,
        .pin_d3 = d_camera_D3_IO,
        .pin_d2 = d_camera_D2_IO,
        .pin_d1 = d_camera_D1_IO,
        .pin_d0 = d_camera_D0_IO,
        .pin_vsync = d_camera_VSYNC_IO,
        .pin_href = d_camera_HREF_IO,
        .pin_pclk = d_camera_PCLK_IO,
        .xclk_freq_hz = d_camera_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = pixformat,
        .frame_size = framesize,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = WDRIVER_I2C_PORT,
    };

    ret = d_camera_err_to_int(esp_camera_init(&camera_config));
    if (ret != 0) {
        D_LOGE(TAG, "摄像头初始化失败, ret=%d, pixformat=%d, frame_size=%d",
                    ret,
                    (int)pixformat,
                    (int)framesize);
        return ret;
    }

    s_camera_pixformat = pixformat;
    s_camera_framesize = framesize;
    s_camera_inited = 1u;
    d_camera_apply_preview_tuning(pixformat);
    d_camera_log_sensor_info();
    D_LOGI(TAG, "摄像头初始化成功, frame_size=%d, pixel_format=%d",
                (int)framesize,
                (int)pixformat);
    return 0;
}

/**
 * @brief 重建 esp-camera 到指定模式。
 *
 * 如果当前模式已经匹配则直接返回。否则先释放旧实例，再用目标格式重新
 * 初始化，这样 HAL 的 jpeg_mode、DMA 长度和 sensor 输出格式保持一致。
 */
static int d_camera_reinit_mode(pixformat_t pixformat, framesize_t framesize)
{
    if (s_camera_inited != 0u &&
        s_camera_pixformat == pixformat &&
        s_camera_framesize == framesize) {
        return 0;
    }

    if (s_camera_inited != 0u) {
        int ret = d_camera_err_to_int(esp_camera_deinit());
        if (ret != 0) {
            D_LOGE(TAG, "摄像头模式切换前释放失败, ret=%d", ret);
            return ret;
        }
        s_camera_inited = 0u;
    }

    return d_camera_init_mode(pixformat, framesize);
}

int d_camera_init(void)
{
    if (s_camera_inited != 0u) {
        D_LOGI(TAG, "摄像头已初始化");
        return 0;
    }

    return d_camera_init_mode(PIXFORMAT_RGB565, D_CAMERA_PREVIEW_FRAME_SIZE);
}

int d_camera_deinit(void)
{
    int ret = d_camera_err_to_int(esp_camera_deinit());
    if (ret == 0) {
        (void)d_camera_set_pwdn_level(1);
        D_LOGI(TAG, "摄像头驱动释放成功");
    } else {
        D_LOGE(TAG, "摄像头驱动释放失败, ret=%d", ret);
    }

    s_camera_inited = 0u;
    s_camera_pixformat = PIXFORMAT_RGB565;
    s_camera_framesize = D_CAMERA_PREVIEW_FRAME_SIZE;
    return ret;
}

int d_camera_set_rgb565_mode(void)
{
    if (s_camera_inited == 0u) {
        D_LOGE(TAG, "切换 RGB565 模式失败: 摄像头未初始化");
        return -1;
    }
    int ret = d_camera_reinit_mode(PIXFORMAT_RGB565, D_CAMERA_PREVIEW_FRAME_SIZE);
    if (ret != 0) {
        D_LOGE(TAG, "切换 RGB565 模式失败, ret=%d", ret);
        return ret;
    }
    D_LOGI(TAG, "摄像头已切换到 RGB565 预览模式, frame_size=%d",
                D_CAMERA_PREVIEW_FRAME_SIZE);
    return 0;
}

int d_camera_set_jpeg_mode(void)
{
    if (s_camera_inited == 0u) {
        D_LOGE(TAG, "切换 JPEG 模式失败: 摄像头未初始化");
        return -1;
    }
    int ret = d_camera_reinit_mode(PIXFORMAT_JPEG, D_CAMERA_CAPTURE_FRAME_SIZE);
    if (ret != 0) {
        D_LOGE(TAG, "切换 JPEG 模式失败, ret=%d", ret);
        return ret;
    }
    D_LOGI(TAG, "摄像头已切换到 JPEG 拍照模式, frame_size=%d",
                D_CAMERA_CAPTURE_FRAME_SIZE);
    return 0;
}

int d_camera_is_initialized(void)
{
    return s_camera_inited != 0u ? 1 : 0;
}

camera_fb_t *d_camera_get_frame(void)
{
    if (s_camera_inited == 0u) {
        D_LOGE(TAG, "摄像头取帧失败: 摄像头未初始化");
        return NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        D_LOGE(TAG, "摄像头取帧失败");
        return NULL;
    }

    /*
     * 预览模式会高频取帧，成功路径不打印日志，避免串口输出拖低帧率。
     * 调试单帧尺寸时可在调用侧按需打印。
     */
    return fb;
}

void d_camera_return_frame(camera_fb_t *fb)
{
    if (fb == NULL) {
        D_LOGW(TAG, "摄像头归还空帧，忽略");
        return;
    }

    esp_camera_fb_return(fb);
}

sensor_t *d_camera_get_sensor(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        D_LOGE(TAG, "获取摄像头 sensor 失败");
    }

    return sensor;
}
