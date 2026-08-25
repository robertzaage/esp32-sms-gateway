idf_component_register(
    SRCS "at_engine.c" "at_parser.c" "at_protocol.c" "at_router.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
