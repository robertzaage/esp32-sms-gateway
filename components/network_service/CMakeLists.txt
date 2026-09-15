idf_component_register(
    SRCS "network_service.c" "provisioning_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer esp_http_server cjson gateway_security lwip
)
