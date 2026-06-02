idf_component_register(
    SRCS
        "beeper.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2s
)
