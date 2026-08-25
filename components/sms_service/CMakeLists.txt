idf_component_register(
    SRCS "sms_service.c"
    INCLUDE_DIRS "include"
    REQUIRES at_engine sms_codec sms_store esp_timer esp_system
)
