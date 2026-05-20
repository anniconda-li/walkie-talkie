file(GLOB_RECURSE SOURCES ./src/*.c)
list(APPEND SOURCES
    ./src/ui/assets/intercom.c
    ./src/ui/assets/ai.c
    ./src/ui/assets/settings.c
    ./src/ui/assets/logo.c
)
idf_component_register(
    SRCS 
        ${SOURCES}
                    
    INCLUDE_DIRS 
        "inc"
        "inc/ui"

    REQUIRES 
        heap
        service
        lvgl__lvgl
        osal

    )
