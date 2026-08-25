#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool overcurrent;
    bool cutoff_latched;
    uint32_t overcurrent_events;
    uint32_t power_cutoffs;
} gateway_board_power_diagnostics_t;

/** Initialize the ESP32-S3-USB-OTG board for native USB host operation. */
esp_err_t gateway_board_init(void);

/** Enable or disable the configured Type-A host VBUS source. */
esp_err_t gateway_board_usb_host_power_set(bool enabled);

/** True when firmware owns a board-provided Type-A VBUS source and can cycle it. */
bool gateway_board_usb_host_power_control_available(void);

/** Human-readable configured Type-A VBUS source (USB_DEV, battery_boost, or external/off). */
const char *gateway_board_usb_host_power_source_name(void);

/** Return true while the board's USB over-current signal is asserted. */
bool gateway_board_usb_overcurrent(void);

esp_err_t gateway_board_power_diagnostics(gateway_board_power_diagnostics_t *out);

/** Green status LED. */
esp_err_t gateway_board_status_led_set(bool on);

#ifdef __cplusplus
}
#endif
