idf_component_register(
    SRCS "dv_portal.c" "dv_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_netif esp_wifi app_update lwip
             dc_wifi dc_source dc_bambu dc_moonraker dv_policy dv_motor dc_evlog
)
