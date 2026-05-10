file(GLOB_RECURSE SOURCES ./src/*.c)
idf_component_register(
    SRCS 
        ${SOURCES}
                    
    INCLUDE_DIRS 
        "inc"

    REQUIRES 
        device_driver
        espressif__esp_lvgl_port
        esp_lcd
        espressif__esp_lcd_touch
        heap
        lvgl__lvgl
        osal

    )
