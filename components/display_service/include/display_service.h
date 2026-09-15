#pragma once

#include "esp_err.h"
#include "network_service.h"

esp_err_t display_service_init(void);
void display_service_network_event(const network_service_snapshot_t *snapshot, void *user_ctx);
