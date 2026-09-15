#pragma once

#include "esp_err.h"

/** Starts the one-time WPA2 SoftAP browser portal. */
esp_err_t provisioning_portal_start(const char *ssid, const char *passphrase);
/** Stops the portal and its captive-DNS responder. Safe when not started. */
void provisioning_portal_stop(void);

