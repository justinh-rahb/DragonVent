idf_component_register(
    SRCS "dv_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_netif esp_wifi json
             dc_wifi dc_source dc_bambu dc_moonraker dc_portal dv_policy dv_motor dv_rgb dc_evlog
)
