idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES driver dv_board dv_motor dc_wifi dc_source dc_bambu dc_moonraker dc_ui
             dv_policy dv_portal dv_button dv_status_led dc_evlog
)
