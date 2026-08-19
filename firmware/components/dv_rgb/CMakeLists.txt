idf_component_register(
    SRCS "dv_rgb.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_lighting dv_board dv_motor
    PRIV_REQUIRES driver nvs_flash
)
