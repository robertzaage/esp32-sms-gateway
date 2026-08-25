#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "gateway_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool enabled;
    bool connected;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t commands_received;
    uint32_t commands_rejected;
    uint32_t events_dropped;
    uint32_t replay_cursor;
    uint32_t replay_inflight_message_id;
    int last_mqtt_error;
} mqtt_service_diagnostics_t;

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_reconfigure(void);
esp_err_t mqtt_service_get_diagnostics(mqtt_service_diagnostics_t *out);

#ifdef __cplusplus
}
#endif
