file(GLOB_RECURSE SOURCES ./src/*.c)
idf_component_register(
    SRCS 
        ${SOURCES}
                    
    INCLUDE_DIRS 
        "inc"
        "inc/ui"

    REQUIRES 
        service
        lvgl__lvgl
        osal

    )
