idf_component_register(
    SRCS "mqtt_service.c" "mqtt_command_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash sms_codec mqtt cjson esp-tls mbedtls modem_core network_service gateway_settings gateway_security api_common api_idempotency esp_timer
    PRIV_REQUIRES esp_app_format
)
