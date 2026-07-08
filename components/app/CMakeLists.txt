file(GLOB_RECURSE SOURCES ./src/*.c)
list(APPEND SOURCES
    ./src/ui/assets/intercom.c
    ./src/ui/assets/ai.c
    ./src/ui/assets/settings.c
    ./src/ui/assets/logo.c
    ./src/ui/assets/icon_ptt_mic.c
    ./src/ui/assets/icon_ai_mic.c
    ./src/ui/assets/icon_ai_camera.c
    ./src/ui/assets/icon_camera_capture.c
    ./src/ui/assets/icon_camera_upload.c
    ./src/ui/assets/icon_camera_unavailable.c
    ./src/ui/assets/icon_camera_retake.c
    ./src/ui/assets/icon_camera_back.c
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
        device
        freertos
        wdriver
        lvgl__lvgl
        osal
        esp_app_format
        lwip
        nvs_flash

    )
