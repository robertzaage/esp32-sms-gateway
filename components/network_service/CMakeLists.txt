idf_component_register(
    SRCS "network_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer network_provisioning
)
