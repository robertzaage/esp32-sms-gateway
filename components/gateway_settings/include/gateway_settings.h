#pragma once

#include "esp_err.h"
#include "mqtt_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gateway_settings_init(const char *device_id);
esp_err_t gateway_settings_get_mqtt(gateway_mqtt_config_t *out);
esp_err_t gateway_settings_set_mqtt(const gateway_mqtt_config_t *config);

#ifdef __cplusplus
}
#endif
