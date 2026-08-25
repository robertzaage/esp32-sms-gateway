idf_component_register(
    SRCS "modem_core.c"
    INCLUDE_DIRS "include"
    REQUIRES usb_modem_transport at_engine modem_manager board sms_service
)
