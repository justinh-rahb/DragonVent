idf_component_register(
    SRCS "dv_motor.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc dv_board
)
