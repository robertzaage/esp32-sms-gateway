idf_component_register(
    SRCS "gateway_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash heap
    PRIV_REQUIRES espcoredump esp_timer freertos
)
