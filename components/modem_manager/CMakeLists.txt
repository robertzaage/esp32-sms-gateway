idf_component_register(
    SRCS "modem_manager.c" "modem_status.c" "modem_recovery_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES at_engine esp_timer
)
