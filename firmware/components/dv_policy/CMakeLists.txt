idf_component_register(
    SRCS "dv_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash dv_motor dc_source dc_bambu dc_moonraker dc_evlog
)
