idf_component_register(
    SRCS "dv_peer.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_peer dc_source dc_moonraker dc_bambu dv_policy dv_motor esp_app_format esp_netif
)
