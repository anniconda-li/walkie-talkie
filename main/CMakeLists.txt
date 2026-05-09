idf_component_register(
    SRCS
        "main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        app
        bsp
        device_driver
        osal
        service
)
