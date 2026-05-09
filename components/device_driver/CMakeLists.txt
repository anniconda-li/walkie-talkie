file(GLOB_RECURSE SOURCES ./src/*.c)
idf_component_register(
    SRCS
        ${SOURCES}

    INCLUDE_DIRS
        "inc"

    REQUIRES
        bsp
        osal
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_client
        esp_driver_gpio
        esp_adc
        esp_lcd
        espressif__esp32-camera
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_ft5x06
)
