file(GLOB_RECURSE SOURCES ./src/*.c)
idf_component_register(
    SRCS 
        ${SOURCES}
                    
    INCLUDE_DIRS 
        "inc"

    REQUIRES 
        bsp
        espressif__esp_lvgl_port
        espressif__esp_lcd_touch
        lvgl__lvgl
        osal

    )
