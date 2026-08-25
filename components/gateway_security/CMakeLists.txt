idf_component_register(
    SRCS "gateway_security.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_system
)
