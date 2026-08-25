#include "gateway_settings.h"

#include <string.h>
#include "gateway_security.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SETTINGS_NAMESPACE "gateway_cfg"
#define MQTT_KEY "mqtt_v1"
#define SETTINGS_MAGIC 0x4d515454U
#define SETTINGS_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t ha_enabled;
    char broker_uri[MQTT_BROKER_URI_MAX];
    char username[MQTT_USERNAME_MAX];
    char password[MQTT_PASSWORD_MAX];
    char base_topic[MQTT_BASE_TOPIC_MAX];
    char discovery_prefix[MQTT_DISCOVERY_PREFIX_MAX];
    char default_recipient[MQTT_DEFAULT_RECIPIENT_MAX];
    char ca_pem[MQTT_CA_PEM_MAX];
} mqtt_record_t;

static nvs_handle_t s_nvs;
static gateway_mqtt_config_t s_config;
static bool s_initialized;
static SemaphoreHandle_t s_mutex;

static void record_to_config(const mqtt_record_t *r, gateway_mqtt_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = r->enabled != 0;
    c->home_assistant_enabled = r->ha_enabled != 0;
    memcpy(c->broker_uri, r->broker_uri, sizeof(c->broker_uri));
    memcpy(c->username, r->username, sizeof(c->username));
    memcpy(c->password, r->password, sizeof(c->password));
    memcpy(c->base_topic, r->base_topic, sizeof(c->base_topic));
    memcpy(c->discovery_prefix, r->discovery_prefix, sizeof(c->discovery_prefix));
    memcpy(c->default_recipient, r->default_recipient, sizeof(c->default_recipient));
    memcpy(c->ca_pem, r->ca_pem, sizeof(c->ca_pem));
    c->broker_uri[sizeof(c->broker_uri) - 1] = 0;
    c->username[sizeof(c->username) - 1] = 0;
    c->password[sizeof(c->password) - 1] = 0;
    c->base_topic[sizeof(c->base_topic) - 1] = 0;
    c->discovery_prefix[sizeof(c->discovery_prefix) - 1] = 0;
    c->default_recipient[sizeof(c->default_recipient) - 1] = 0;
    c->ca_pem[sizeof(c->ca_pem) - 1] = 0;
}

static void config_to_record(const gateway_mqtt_config_t *c, mqtt_record_t *r)
{
    memset(r, 0, sizeof(*r));
    r->magic = SETTINGS_MAGIC;
    r->version = SETTINGS_VERSION;
    r->enabled = c->enabled ? 1 : 0;
    r->ha_enabled = c->home_assistant_enabled ? 1 : 0;
    memcpy(r->broker_uri, c->broker_uri, sizeof(r->broker_uri));
    memcpy(r->username, c->username, sizeof(r->username));
    memcpy(r->password, c->password, sizeof(r->password));
    memcpy(r->base_topic, c->base_topic, sizeof(r->base_topic));
    memcpy(r->discovery_prefix, c->discovery_prefix, sizeof(r->discovery_prefix));
    memcpy(r->default_recipient, c->default_recipient, sizeof(r->default_recipient));
    memcpy(r->ca_pem, c->ca_pem, sizeof(r->ca_pem));
}

esp_err_t gateway_settings_init(const char *device_id)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return err;
    mqtt_record_t record;
    size_t size = sizeof(record);
    err = nvs_get_blob(s_nvs, MQTT_KEY, &record, &size);
    if (err == ESP_OK && size == sizeof(record) && record.magic == SETTINGS_MAGIC && record.version == SETTINGS_VERSION) {
        record_to_config(&record, &s_config);
        gateway_security_wipe(&record, sizeof(record));
        if (gateway_mqtt_config_validate(&s_config) != ESP_OK) {
            gateway_security_wipe(&s_config, sizeof(s_config));
            return ESP_ERR_INVALID_STATE;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        gateway_mqtt_config_defaults(&s_config, device_id);
    } else {
        gateway_security_wipe(&record, sizeof(record));
        return err == ESP_OK ? ESP_ERR_INVALID_VERSION : err;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t gateway_settings_get_mqtt(gateway_mqtt_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_config;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t gateway_settings_set_mqtt(const gateway_mqtt_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gateway_mqtt_config_validate(config);
    if (err != ESP_OK) return err;
    mqtt_record_t record;
    config_to_record(config, &record);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        gateway_security_wipe(&record, sizeof(record));
        return ESP_ERR_TIMEOUT;
    }
    err = nvs_set_blob(s_nvs, MQTT_KEY, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    gateway_security_wipe(&record, sizeof(record));
    if (err == ESP_OK) {
        gateway_security_wipe(&s_config, sizeof(s_config));
        s_config = *config;
    }
    xSemaphoreGive(s_mutex);
    return err;
}
