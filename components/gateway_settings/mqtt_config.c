#include "mqtt_config.h"

#include <stdio.h>
#include <string.h>

static bool e164_valid(const char *value)
{
    if (value == NULL || value[0] != '+') return false;
    size_t digits = 0;
    for (size_t i = 1; value[i]; ++i) {
        if (value[i] < '0' || value[i] > '9' || (i == 1 && value[i] == '0')) return false;
        if (++digits > 15) return false;
    }
    return digits >= 2;
}

static bool topic_valid(const char *value, size_t max_len)
{
    if (value == NULL) return false;
    const size_t len = strlen(value);
    if (len == 0 || len >= max_len || value[0] == '/' || value[len - 1] == '/') return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e || c == '#' || c == '+') return false;
    }
    return true;
}

bool gateway_mqtt_uri_is_tls(const char *uri)
{
    return uri != NULL && strncmp(uri, "mqtts://", 8) == 0;
}

void gateway_mqtt_config_defaults(gateway_mqtt_config_t *config, const char *device_id)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->home_assistant_enabled = true;
    snprintf(config->base_topic, sizeof(config->base_topic), "sms-gateway/%s",
             device_id != NULL && device_id[0] ? device_id : "gateway");
    snprintf(config->discovery_prefix, sizeof(config->discovery_prefix), "homeassistant");
}

esp_err_t gateway_mqtt_config_validate(const gateway_mqtt_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (!topic_valid(config->base_topic, sizeof(config->base_topic)) ||
        !topic_valid(config->discovery_prefix, sizeof(config->discovery_prefix))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->default_recipient[0] != '\0' && !e164_valid(config->default_recipient)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->enabled) return ESP_OK;
    const bool mqtt = strncmp(config->broker_uri, "mqtt://", 7) == 0;
    const bool mqtts = gateway_mqtt_uri_is_tls(config->broker_uri);
    if ((!mqtt && !mqtts) || strchr(config->broker_uri, '@') != NULL || strchr(config->broker_uri, ' ') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *host = config->broker_uri + (mqtts ? 8 : 7);
    if (*host == '\0' || *host == '/') return ESP_ERR_INVALID_ARG;
    if (!mqtts && config->ca_pem[0] != '\0') return ESP_ERR_INVALID_ARG;
    if (config->ca_pem[0] != '\0' && strstr(config->ca_pem, "-----BEGIN CERTIFICATE-----") == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
