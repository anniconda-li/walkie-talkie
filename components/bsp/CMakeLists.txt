file(GLOB_RECURSE SOURCES ./src/*.c)
idf_component_register(
    SRCS 
        ${SOURCES}
                    
    INCLUDE_DIRS 
        "inc"

    REQUIRES
        esp_driver_uart
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_spi
        esp_lcd
        espressif__esp32-camera
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_ft5x06
        osal
    )
