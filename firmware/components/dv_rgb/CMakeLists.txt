idf_component_register(
    SRCS "dv_rgb.c"
    INCLUDE_DIRS "include"
    REQUIRES dv_board dv_motor
    PRIV_REQUIRES led_strip
)
