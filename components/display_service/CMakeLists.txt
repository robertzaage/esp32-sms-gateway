idf_component_register(
    SRCS "display_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio network_service
)
