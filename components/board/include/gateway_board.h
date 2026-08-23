#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the ESP32-S3-USB-OTG board for native USB host operation. */
esp_err_t gateway_board_init(void);

/** Enable or disable the configured Type-A host VBUS source. */
esp_err_t gateway_board_usb_host_power_set(bool enabled);

/** Return true while the board's USB over-current signal is asserted. */
bool gateway_board_usb_overcurrent(void);

/** Green status LED. */
esp_err_t gateway_board_status_led_set(bool on);

#ifdef __cplusplus
}
#endif
