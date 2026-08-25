idf_component_register(
    SRCS "usb_modem_transport.c" "usb_modem_descriptor.c" "huawei_mode_switch.c"
    INCLUDE_DIRS "include"
    REQUIRES usb usb_host_cdc_acm
)
