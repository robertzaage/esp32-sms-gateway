idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_app_format nvs_flash board modem_core
)
