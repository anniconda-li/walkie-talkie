/**
 * @file driver_camera.c
 * @brief 基于 esp32-camera 组件的 BSP 摄像头驱动实现。
 */
#include "driver_camera.h"

#include "driver_config.h"
#include "bsp_i2c.h"
#include "driver_pca9557.h"
#include "esp_idf_version.h"

#include <stdint.h>

// #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)
// #error "bsp_camera requires ESP-IDF >= 5.4 so esp32-camera uses the new SCCB I2C driver and can reuse bsp_i2c."
// #endif

/**
 * @brief 摄像头日志标签。
 */
static const char *TAG = "driver_camera";

/**
 * @brief 摄像头 XCLK 频率。
 */
#define driver_camera_XCLK_FREQ_HZ 24000000

/** @brief 本地预览使用的帧尺寸。 */
#define DRIVER_CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_240X240

/** @brief 拍照上传使用的 JPEG 帧尺寸，第一版使用 QVGA 兼顾兼容性和数据量。 */
#define DRIVER_CAMERA_CAPTURE_FRAME_SIZE FRAMESIZE_QVGA

/** @brief 摄像头 driver 是否已成功初始化。 */
static uint8_t s_camera_inited = 0u;

/** @brief 当前摄像头输出格式，用于避免重复切换 sensor 模式。 */
static pixformat_t s_camera_pixformat = PIXFORMAT_RGB565;

/** @brief 当前摄像头输出尺寸，用于避免重复切换 sensor 模式。 */
static framesize_t s_camera_framesize = DRIVER_CAMERA_PREVIEW_FRAME_SIZE;

/**
 * @brief 将底层驱动错误码转换为 BSP 通用 int 返回值。
 *
 * @param[in] ret 底层驱动错误码。
 * @return 0 表示成功；其他错误码转换为负值。
 */
static int driver_camera_err_to_int(int ret)
{
    return (ret == 0) ? 0 : ((ret < 0) ? ret : -ret);
}

int driver_camera_init(void)
{
    if (s_camera_inited != 0u) {
        DRIVER_LOGI(TAG, "摄像头已初始化");
        return 0;
    }

    if (bsp_i2c_get_bus_handle() == NULL) {
        DRIVER_LOGE(TAG, "摄像头初始化失败: I2C 未初始化");
        return -1;
    }

    if (driver_pca9557_is_initialized() != 0) {
        (void)driver_pca9557_set_camera_pwdn(PCA9557_LEVEL_LOW);
    }

    camera_config_t camera_config = {
        .pin_pwdn = driver_camera_PWDN_IO,
        .pin_reset = driver_camera_RESET_IO,
        .pin_xclk = driver_camera_XCLK_IO,
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d7 = driver_camera_D7_IO,
        .pin_d6 = driver_camera_D6_IO,
        .pin_d5 = driver_camera_D5_IO,
        .pin_d4 = driver_camera_D4_IO,
        .pin_d3 = driver_camera_D3_IO,
        .pin_d2 = driver_camera_D2_IO,
        .pin_d1 = driver_camera_D1_IO,
        .pin_d0 = driver_camera_D0_IO,
        .pin_vsync = driver_camera_VSYNC_IO,
        .pin_href = driver_camera_HREF_IO,
        .pin_pclk = driver_camera_PCLK_IO,
        .xclk_freq_hz = driver_camera_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = DRIVER_CAMERA_PREVIEW_FRAME_SIZE,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = BSP_I2C_PORT,
    };

    int ret = driver_camera_err_to_int(esp_camera_init(&camera_config));
    if (ret != 0) {
        DRIVER_LOGE(TAG, "摄像头初始化失败, ret=%d", ret);
        return ret;
    }

    s_camera_pixformat = PIXFORMAT_RGB565;
    s_camera_framesize = DRIVER_CAMERA_PREVIEW_FRAME_SIZE;
    s_camera_inited = 1u;
    DRIVER_LOGI(TAG, "摄像头初始化成功, frame_size=%d, pixel_format=%d",
             DRIVER_CAMERA_PREVIEW_FRAME_SIZE, PIXFORMAT_RGB565);
    return 0;
}

int driver_camera_deinit(void)
{
    int ret = driver_camera_err_to_int(esp_camera_deinit());
    if (ret == 0) {
        if (driver_pca9557_is_initialized() != 0) {
            (void)driver_pca9557_set_camera_pwdn(PCA9557_LEVEL_HIGH);
        }
        DRIVER_LOGI(TAG, "摄像头驱动释放成功");
    } else {
        DRIVER_LOGE(TAG, "摄像头驱动释放失败, ret=%d", ret);
    }

    s_camera_inited = 0u;
    s_camera_pixformat = PIXFORMAT_RGB565;
    s_camera_framesize = DRIVER_CAMERA_PREVIEW_FRAME_SIZE;
    return ret;
}

int driver_camera_set_rgb565_mode(void)
{
    if (s_camera_inited == 0u) {
        DRIVER_LOGE(TAG, "切换 RGB565 模式失败: 摄像头未初始化");
        return -1;
    }
    if (s_camera_pixformat == PIXFORMAT_RGB565 &&
        s_camera_framesize == DRIVER_CAMERA_PREVIEW_FRAME_SIZE) {
        return 0;
    }

    /*
     * 运行时切换格式由 esp32-camera sensor 驱动完成。若后续实测某个传感器
     * 不支持稳定切换，可把这里内部改成 deinit/init 重建，service/app 接口不变。
     */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        DRIVER_LOGE(TAG, "切换 RGB565 模式失败: sensor 为空");
        return -2;
    }

    int ret = driver_camera_err_to_int(sensor->set_pixformat(sensor, PIXFORMAT_RGB565));
    if (ret == 0) {
        ret = driver_camera_err_to_int(sensor->set_framesize(sensor,
                                                             DRIVER_CAMERA_PREVIEW_FRAME_SIZE));
    }
    if (ret != 0) {
        DRIVER_LOGE(TAG, "切换 RGB565 模式失败, ret=%d", ret);
        return ret;
    }

    s_camera_pixformat = PIXFORMAT_RGB565;
    s_camera_framesize = DRIVER_CAMERA_PREVIEW_FRAME_SIZE;
    DRIVER_LOGI(TAG, "摄像头已切换到 RGB565 预览模式, frame_size=%d",
                DRIVER_CAMERA_PREVIEW_FRAME_SIZE);
    return 0;
}

int driver_camera_set_jpeg_mode(void)
{
    if (s_camera_inited == 0u) {
        DRIVER_LOGE(TAG, "切换 JPEG 模式失败: 摄像头未初始化");
        return -1;
    }
    if (s_camera_pixformat == PIXFORMAT_JPEG &&
        s_camera_framesize == DRIVER_CAMERA_CAPTURE_FRAME_SIZE) {
        return 0;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        DRIVER_LOGE(TAG, "切换 JPEG 模式失败: sensor 为空");
        return -2;
    }

    int ret = driver_camera_err_to_int(sensor->set_pixformat(sensor, PIXFORMAT_JPEG));
    if (ret == 0) {
        ret = driver_camera_err_to_int(sensor->set_framesize(sensor,
                                                             DRIVER_CAMERA_CAPTURE_FRAME_SIZE));
    }
    if (ret == 0 && sensor->set_quality != NULL) {
        ret = driver_camera_err_to_int(sensor->set_quality(sensor, 12));
    }
    if (ret != 0) {
        DRIVER_LOGE(TAG, "切换 JPEG 模式失败, ret=%d", ret);
        return ret;
    }

    s_camera_pixformat = PIXFORMAT_JPEG;
    s_camera_framesize = DRIVER_CAMERA_CAPTURE_FRAME_SIZE;
    DRIVER_LOGI(TAG, "摄像头已切换到 JPEG 拍照模式, frame_size=%d",
                DRIVER_CAMERA_CAPTURE_FRAME_SIZE);
    return 0;
}

int driver_camera_is_initialized(void)
{
    return s_camera_inited != 0u ? 1 : 0;
}

camera_fb_t *driver_camera_get_frame(void)
{
    if (s_camera_inited == 0u) {
        DRIVER_LOGE(TAG, "摄像头取帧失败: 摄像头未初始化");
        return NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        DRIVER_LOGE(TAG, "摄像头取帧失败");
        return NULL;
    }

    /*
     * 预览模式会高频取帧，成功路径不打印日志，避免串口输出拖低帧率。
     * 调试单帧尺寸时可在调用侧按需打印。
     */
    return fb;
}

void driver_camera_return_frame(camera_fb_t *fb)
{
    if (fb == NULL) {
        DRIVER_LOGW(TAG, "摄像头归还空帧，忽略");
        return;
    }

    esp_camera_fb_return(fb);
}

sensor_t *driver_camera_get_sensor(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        DRIVER_LOGE(TAG, "获取摄像头 sensor 失败");
    }

    return sensor;
}
