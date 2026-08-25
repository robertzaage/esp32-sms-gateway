idf_component_register(
    SRCS "app_main.c"
    REQUIRES board modem_core gateway_security api_idempotency network_service gateway_settings mqtt_service api_server ota_service app_update nvs_flash
)
