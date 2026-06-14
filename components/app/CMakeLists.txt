file(GLOB_RECURSE SOURCES ./src/*.c)
option(APP_USE_CUSTOM_UI_FONTS "Build with generated Chinese LVGL UI fonts" OFF)
if(NOT APP_USE_CUSTOM_UI_FONTS)
    list(REMOVE_ITEM SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/ui_font_14.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/ui_font_16.c"
    )
endif()
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
        wdriver
        lvgl__lvgl
        osal
        esp_app_format
        nvs_flash

    )

if(APP_USE_CUSTOM_UI_FONTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_USE_CUSTOM_UI_FONTS=1)
endif()
