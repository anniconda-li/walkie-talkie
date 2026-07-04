file(GLOB_RECURSE SOURCES ./src/*.c)
list(REMOVE_ITEM SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d_inmp441.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d_max98357a.c"
)
idf_component_register(
    SRCS
        ${SOURCES}

    INCLUDE_DIRS
        "inc"

    REQUIRES
        wdriver
        osal
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_client
        heap
        esp_driver_gpio
        esp_driver_ledc
        esp_adc
        esp_lcd
        espressif__esp32-camera
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_ft5x06
)
