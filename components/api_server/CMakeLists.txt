idf_component_register(
    SRCS "api_server.c"
    INCLUDE_DIRS "include"
    REQUIRES board sms_codec esp_http_server cjson modem_core network_service gateway_security gateway_settings mqtt_service api_common api_idempotency ota_service esp_timer app_update
)
