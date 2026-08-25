#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODEM_USB_EVENT_HOST_READY = 0,
    MODEM_USB_EVENT_DEVICE_FOUND,
    MODEM_USB_EVENT_PROBING,
    MODEM_USB_EVENT_AT_READY,
    MODEM_USB_EVENT_DISCONNECTED,
    MODEM_USB_EVENT_ERROR,
} modem_usb_event_t;

typedef struct {
    bool host_ready;
    bool device_present;
    bool at_ready;
    uint16_t vid;
    uint16_t pid;
    uint8_t interface_number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint8_t interrupt_in_ep;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t probe_attempts;
    uint32_t open_failures;
    uint32_t mode_switch_attempts;
    uint32_t mode_switch_successes;
    uint32_t mode_switch_failures;
    esp_err_t last_error;
} modem_usb_diagnostics_t;

typedef void (*modem_usb_event_callback_t)(modem_usb_event_t event,
                                           const modem_usb_diagnostics_t *diagnostics,
                                           void *user_ctx);

typedef void (*modem_usb_rx_callback_t)(const uint8_t *data, size_t len, void *user_ctx);

/**
 * Start the USB Host library, CDC-like modem driver, and Huawei AT-port probe.
 * This function is idempotent; a second call returns ESP_ERR_INVALID_STATE.
 */
esp_err_t modem_usb_transport_start(modem_usb_event_callback_t event_cb, void *user_ctx);

/** Set the non-blocking consumer for raw bytes received from the selected CDC interface. */
esp_err_t modem_usb_transport_set_rx_callback(modem_usb_rx_callback_t rx_cb, void *user_ctx);

/** Send raw bytes to the selected AT interface after AT probing succeeded. */
esp_err_t modem_usb_transport_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/** True only while a selected interface is open and has answered an AT probe. */
bool modem_usb_transport_is_ready(void);

/** Copy the latest transport diagnostics snapshot. */
esp_err_t modem_usb_transport_get_diagnostics(modem_usb_diagnostics_t *out);

#ifdef __cplusplus
}
#endif
