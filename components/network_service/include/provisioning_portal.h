#pragma once

#include "esp_err.h"

/**
 * Start the browser setup portal (HTTP on port 80 plus captive DNS) on the
 * SoftAP. The caller must have stopped any other server bound to port 80.
 */
esp_err_t provisioning_portal_start(void);
/** Stops the portal and its captive-DNS responder. Safe when not started. */
void provisioning_portal_stop(void);
