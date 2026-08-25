#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_BROKER_URI_MAX 256
#define MQTT_USERNAME_MAX 96
#define MQTT_PASSWORD_MAX 160
#define MQTT_BASE_TOPIC_MAX 128
#define MQTT_DISCOVERY_PREFIX_MAX 64
#define MQTT_DEFAULT_RECIPIENT_MAX 24
#define MQTT_CA_PEM_MAX 3072

typedef struct {
    bool enabled;
    char broker_uri[MQTT_BROKER_URI_MAX];
    char username[MQTT_USERNAME_MAX];
    char password[MQTT_PASSWORD_MAX];
    char base_topic[MQTT_BASE_TOPIC_MAX];
    bool home_assistant_enabled;
    char discovery_prefix[MQTT_DISCOVERY_PREFIX_MAX];
    char default_recipient[MQTT_DEFAULT_RECIPIENT_MAX];
    char ca_pem[MQTT_CA_PEM_MAX];
} gateway_mqtt_config_t;

void gateway_mqtt_config_defaults(gateway_mqtt_config_t *config, const char *device_id);
esp_err_t gateway_mqtt_config_validate(const gateway_mqtt_config_t *config);
bool gateway_mqtt_uri_is_tls(const char *uri);

#ifdef __cplusplus
}
#endif
