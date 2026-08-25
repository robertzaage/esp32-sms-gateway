idf_component_register(
    SRCS "gateway_settings.c" "mqtt_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash gateway_security freertos
)
