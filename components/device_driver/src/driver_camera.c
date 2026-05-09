/**
 * @file driver_camera.c
 * @brief 基于 esp32-camera 组件的 BSP 摄像头驱动实现。
 */
#include "driver_camera.h"

#include "bsp_common.h"
#include "bsp_i2c.h"
#include "esp_idf_version.h"

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
    if (bsp_i2c_get_bus_handle() == NULL) {
        BSP_LOGE(TAG, "摄像头初始化失败: I2C 未初始化");
        return -1;
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
        .frame_size = FRAMESIZE_240X240,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = BSP_I2C_PORT,
    };

    int ret = driver_camera_err_to_int(esp_camera_init(&camera_config));
    if (ret != 0) {
        BSP_LOGE(TAG, "摄像头初始化失败, ret=%d", ret);
        return ret;
    }

    BSP_LOGI(TAG, "摄像头初始化成功, frame_size=%d, pixel_format=%d",
             FRAMESIZE_240X240, PIXFORMAT_RGB565);
    return 0;
}

int driver_camera_deinit(void)
{
    int ret = driver_camera_err_to_int(esp_camera_deinit());
    if (ret == 0) {
        BSP_LOGI(TAG, "摄像头驱动释放成功");
    } else {
        BSP_LOGE(TAG, "摄像头驱动释放失败, ret=%d", ret);
    }

    return ret;
}

camera_fb_t *driver_camera_get_frame(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        BSP_LOGE(TAG, "摄像头取帧失败");
        return NULL;
    }

    BSP_LOGI(TAG, "摄像头取帧成功, width=%u, height=%u, len=%u",
             (unsigned int)fb->width,
             (unsigned int)fb->height,
             (unsigned int)fb->len);
    return fb;
}

void driver_camera_return_frame(camera_fb_t *fb)
{
    if (fb == NULL) {
        BSP_LOGW(TAG, "摄像头归还空帧，忽略");
        return;
    }

    esp_camera_fb_return(fb);
    BSP_LOGI(TAG, "摄像头帧缓存已归还");
}

sensor_t *driver_camera_get_sensor(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        BSP_LOGE(TAG, "获取摄像头 sensor 失败");
    }

    return sensor;
}
