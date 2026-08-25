idf_component_register(
    SRCS "ota_service.c" "ota_version_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_app_format esp_partition mbedtls
)
