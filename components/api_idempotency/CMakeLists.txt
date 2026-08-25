idf_component_register(
    SRCS "api_idempotency.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash gateway_security freertos
)
