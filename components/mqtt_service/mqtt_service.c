#include "mqtt_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_common.h"
#include "api_idempotency.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_security.h"
#include "modem_core.h"
#include "mqtt_command_policy.h"
#include "network_service.h"
#include "sms_codec.h"
#include "nvs.h"

#define MQTT_WORK_QUEUE_DEPTH 8
#define MQTT_TOPIC_MAX 192
#define MQTT_PAYLOAD_MAX 4608
#define MQTT_STATUS_PERIOD_MS 30000
#define MQTT_WORKER_STACK 9216
#define MQTT_REPLAY_NAMESPACE "mqtt_rt"
#define MQTT_REPLAY_CURSOR_KEY "rx_cursor"
#define MQTT_REPLAY_ACK_TIMEOUT_MS 60000

static const char *TAG = "mqtt";

typedef enum {
    MQTT_WORK_CONNECTED = 0,
    MQTT_WORK_DATA,
    MQTT_WORK_SMS_EVENT,
    MQTT_WORK_REPLAY,
    MQTT_WORK_REPLAY_ACK,
} mqtt_work_type_t;

typedef struct {
    mqtt_work_type_t type;
    sms_service_event_t sms_event;
    char topic[MQTT_TOPIC_MAX];
    size_t payload_len;
    bool retained;
    bool duplicate;
    int qos;
    uint32_t message_id;
    char payload[MQTT_PAYLOAD_MAX + 1];
} mqtt_work_t;

typedef struct {
    char topic[MQTT_TOPIC_MAX];
    size_t total;
    size_t received;
    bool retained;
    bool duplicate;
    int qos;
    char data[MQTT_PAYLOAD_MAX + 1];
} mqtt_rx_accumulator_t;

typedef struct {
    uint32_t id;
    sms_direction_t direction;
    sms_message_status_t status;
    char sender[SMS_MAX_ADDRESS_LENGTH];
    char recipient[SMS_MAX_ADDRESS_LENGTH];
    char service_center_timestamp[40];
    char text[SMS_MESSAGE_TEXT_MAX];
} mqtt_sms_event_snapshot_t;

_Static_assert(sizeof(mqtt_sms_event_snapshot_t) <= MQTT_PAYLOAD_MAX, "MQTT SMS event snapshot exceeds payload buffer");

static esp_mqtt_client_handle_t s_client;
/*
 * ESP-MQTT intentionally retains broker.verification.certificate by pointer.
 * Keep a separately owned PEM allocation alive for exactly the client lifetime;
 * never point the client at a stack snapshot or a config buffer that may be
 * replaced while the client is running.
 */
static char *s_active_ca_pem;
static gateway_mqtt_config_t s_config;
static mqtt_service_diagnostics_t s_diag;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_client_mutex;
static SemaphoreHandle_t s_config_mutex;
static SemaphoreHandle_t s_runtime_mutex;
static TaskHandle_t s_worker;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static mqtt_rx_accumulator_t s_rx;
static gateway_rate_limiter_t s_sms_limiter;
static nvs_handle_t s_runtime_nvs;
static uint32_t s_replay_cursor;
static uint32_t s_replay_message_id;
static int s_replay_packet_id;
static int64_t s_replay_started_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static esp_mqtt_client_handle_t active_client_get(void)
{
    esp_mqtt_client_handle_t client;
    portENTER_CRITICAL(&s_state_lock);
    client = s_client;
    portEXIT_CRITICAL(&s_state_lock);
    return client;
}

static void active_client_set(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_state_lock);
    s_client = client;
    portEXIT_CRITICAL(&s_state_lock);
}

static char *owned_ca_duplicate(const char *pem)
{
    if (pem == NULL || pem[0] == '\0') return NULL;
    const size_t len = strlen(pem);
    char *copy = malloc(len + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, pem, len + 1U);
    return copy;
}

static void owned_ca_free(char **pem)
{
    if (pem == NULL || *pem == NULL) return;
    gateway_security_wipe(*pem, strlen(*pem));
    free(*pem);
    *pem = NULL;
}

static void diag_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_lock);
    if (connected && !s_diag.connected) ++s_diag.connects;
    if (!connected && s_diag.connected) ++s_diag.disconnects;
    s_diag.connected = connected;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool connected(void)
{
    bool value;
    portENTER_CRITICAL(&s_state_lock); value = s_diag.connected; portEXIT_CRITICAL(&s_state_lock);
    return value;
}

static bool config_snapshot(gateway_mqtt_config_t *out)
{
    if (out == NULL || s_config_mutex == NULL) return false;
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    *out = s_config;
    xSemaphoreGive(s_config_mutex);
    return true;
}

static void topic(char out[MQTT_TOPIC_MAX], const char *suffix)
{
    gateway_mqtt_config_t config = {0};
    if (!config_snapshot(&config)) { out[0] = '\0'; return; }
    snprintf(out, MQTT_TOPIC_MAX, "%s/%s", config.base_topic, suffix);
    gateway_security_wipe(&config, sizeof(config));
}

static int publish_raw(const char *topic_name, const char *payload, int qos, int retain)
{
    if (s_client_mutex == NULL || topic_name == NULL || payload == NULL) return -1;
    if (xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return -1;
    esp_mqtt_client_handle_t client = active_client_get();
    const int id = client != NULL && connected()
                       ? esp_mqtt_client_publish(client, topic_name, payload, 0, qos, retain)
                       : -1;
    xSemaphoreGive(s_client_mutex);
    return id;
}

static void publish_json_object(const char *topic_name, cJSON *obj, int qos, int retain)
{
    char *encoded = cJSON_PrintUnformatted(obj);
    if (encoded == NULL) return;
    (void)publish_raw(topic_name, encoded, qos, retain);
    gateway_security_wipe(encoded, strlen(encoded));
    cJSON_free(encoded);
}

static unsigned current_sms_queue_depth(void)
{
    unsigned count_queue = 0;
    uint32_t cursor = 0;
    for (unsigned scanned = 0; scanned < SMS_STORE_MAX_RECORDS; ++scanned) {
        sms_message_t *message = calloc(1, sizeof(*message));
        if (message == NULL) break;
        size_t count = 0;
        if (modem_core_sms_list(cursor, message, 1, &count) != ESP_OK || count == 0) {
            gateway_security_wipe(message, sizeof(*message)); free(message); break;
        }
        cursor = message->id;
        if (message->direction == SMS_DIRECTION_OUTBOUND &&
            (message->status == SMS_MESSAGE_QUEUED || message->status == SMS_MESSAGE_SENDING)) {
            ++count_queue;
        }
        gateway_security_wipe(message, sizeof(*message));
        free(message);
    }
    return count_queue;
}

static void publish_status(void)
{
    modem_manager_snapshot_t modem = {0};
    if (modem_core_manager_snapshot(&modem) != ESP_OK) return;
    char status_topic[MQTT_TOPIC_MAX]; topic(status_topic, "status");
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON_AddStringToObject(root, "modem_state", modem_core_state_name(modem_core_state()));
    cJSON_AddStringToObject(root, "sim", modem_sim_state_name(modem.sim));
    cJSON_AddBoolToObject(root, "registered", modem.registered);
    cJSON_AddBoolToObject(root, "roaming", modem.roaming);
    if (modem.operator_info.name[0]) cJSON_AddStringToObject(root, "operator", modem.operator_info.name);
    else cJSON_AddNullToObject(root, "operator");
    if (modem.signal.rssi == 99) cJSON_AddNullToObject(root, "rssi_dbm");
    else cJSON_AddNumberToObject(root, "rssi_dbm", modem.signal.rssi_dbm);
    cJSON_AddNumberToObject(root, "sms_queue", current_sms_queue_depth());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    publish_json_object(status_topic, root, 1, 1);
    cJSON_Delete(root);
}

static void add_component_common(cJSON *component, const char *platform, const char *name, const char *unique_id)
{
    cJSON_AddStringToObject(component, "p", platform);
    cJSON_AddStringToObject(component, "name", name);
    cJSON_AddStringToObject(component, "unique_id", unique_id);
}

static cJSON *component(cJSON *components, const char *key)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj != NULL) cJSON_AddItemToObject(components, key, obj);
    return obj;
}

static void publish_discovery(void)
{
    gateway_mqtt_config_t config = {0};
    if (!config_snapshot(&config) || !config.home_assistant_enabled) { gateway_security_wipe(&config, sizeof(config)); return; }
    const char *device_id = network_service_device_id();
    const esp_app_desc_t *app = esp_app_get_description();
    char discovery_topic[MQTT_TOPIC_MAX];
    snprintf(discovery_topic, sizeof(discovery_topic), "%s/device/%s/config", config.discovery_prefix, device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON_AddStringToObject(dev, "ids", device_id);
    cJSON_AddStringToObject(dev, "name", "SMS Gateway");
    cJSON_AddStringToObject(dev, "mf", "ESP32 SMS Gateway");
    cJSON_AddStringToObject(dev, "mdl", "ESP32-S3-USB-OTG SMS Gateway");
    cJSON_AddStringToObject(dev, "sw", app->version);
    cJSON *origin = cJSON_AddObjectToObject(root, "o");
    cJSON_AddStringToObject(origin, "name", "esp32-sms-gateway");
    cJSON_AddStringToObject(origin, "sw", app->version);

    char availability[MQTT_TOPIC_MAX]; topic(availability, "availability");
    cJSON_AddStringToObject(root, "availability_topic", availability);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON *cmps = cJSON_AddObjectToObject(root, "cmps");
    char state_topic[MQTT_TOPIC_MAX]; topic(state_topic, "status");
    char unique[96];

#define SENSOR(KEY, NAME, TEMPLATE) do { \
    cJSON *x = component(cmps, KEY); snprintf(unique, sizeof(unique), "%s_%s", device_id, KEY); \
    add_component_common(x, "sensor", NAME, unique); cJSON_AddStringToObject(x, "state_topic", state_topic); \
    cJSON_AddStringToObject(x, "value_template", TEMPLATE); cJSON_AddStringToObject(x, "entity_category", "diagnostic"); \
} while (0)
    SENSOR("modem_state", "Modem state", "{{ value_json.modem_state }}");
    SENSOR("sim_state", "SIM state", "{{ value_json.sim }}");
    SENSOR("operator", "Operator", "{{ value_json.operator | default('unknown', true) }}");
    SENSOR("sms_queue", "SMS queue", "{{ value_json.sms_queue }}");
    cJSON *signal = component(cmps, "signal"); snprintf(unique, sizeof(unique), "%s_signal", device_id);
    add_component_common(signal, "sensor", "Signal strength", unique);
    cJSON_AddStringToObject(signal, "state_topic", state_topic);
    cJSON_AddStringToObject(signal, "value_template", "{{ value_json.rssi_dbm }}");
    cJSON_AddStringToObject(signal, "unit_of_measurement", "dBm");
    cJSON_AddStringToObject(signal, "device_class", "signal_strength");
    cJSON_AddStringToObject(signal, "entity_category", "diagnostic");

    cJSON *reg = component(cmps, "registered"); snprintf(unique, sizeof(unique), "%s_registered", device_id);
    add_component_common(reg, "binary_sensor", "Registered", unique);
    cJSON_AddStringToObject(reg, "state_topic", state_topic);
    cJSON_AddStringToObject(reg, "value_template", "{{ 'ON' if value_json.registered else 'OFF' }}");
    cJSON_AddStringToObject(reg, "payload_on", "ON"); cJSON_AddStringToObject(reg, "payload_off", "OFF");
    cJSON_AddStringToObject(reg, "entity_category", "diagnostic");

    char received_topic[MQTT_TOPIC_MAX]; topic(received_topic, "sms/received");
    cJSON *evt = component(cmps, "incoming_sms"); snprintf(unique, sizeof(unique), "%s_incoming_sms", device_id);
    add_component_common(evt, "event", "Incoming SMS", unique);
    cJSON_AddStringToObject(evt, "state_topic", received_topic);
    cJSON *types = cJSON_AddArrayToObject(evt, "event_types"); cJSON_AddItemToArray(types, cJSON_CreateString("received"));

    char modem_restart[MQTT_TOPIC_MAX]; topic(modem_restart, "modem/restart");
    cJSON *mb = component(cmps, "restart_modem"); snprintf(unique, sizeof(unique), "%s_restart_modem", device_id);
    add_component_common(mb, "button", "Restart modem", unique); cJSON_AddStringToObject(mb, "command_topic", modem_restart);
    cJSON_AddStringToObject(mb, "payload_press", "PRESS"); cJSON_AddStringToObject(mb, "device_class", "restart");
    cJSON_AddStringToObject(mb, "entity_category", "config");

    char gateway_restart[MQTT_TOPIC_MAX]; topic(gateway_restart, "system/reboot");
    cJSON *gb = component(cmps, "restart_gateway"); snprintf(unique, sizeof(unique), "%s_restart_gateway", device_id);
    add_component_common(gb, "button", "Restart gateway", unique); cJSON_AddStringToObject(gb, "command_topic", gateway_restart);
    cJSON_AddStringToObject(gb, "payload_press", "PRESS"); cJSON_AddStringToObject(gb, "device_class", "restart");
    cJSON_AddStringToObject(gb, "entity_category", "config");

    if (config.default_recipient[0]) {
        char notify_topic[MQTT_TOPIC_MAX]; topic(notify_topic, "ha/notify");
        cJSON *notify = component(cmps, "sms_notify"); snprintf(unique, sizeof(unique), "%s_sms_notify", device_id);
        add_component_common(notify, "notify", "SMS", unique); cJSON_AddStringToObject(notify, "command_topic", notify_topic);
        cJSON_AddNumberToObject(notify, "qos", 0); cJSON_AddBoolToObject(notify, "retain", false);
    }
#undef SENSOR
    publish_json_object(discovery_topic, root, 1, 1);
    cJSON_Delete(root);
    gateway_security_wipe(&config, sizeof(config));
}

static void publish_command_result(const char *request_id, bool ok, const char *code, uint32_t message_id, bool replayed)
{
    char result_topic[MQTT_TOPIC_MAX]; topic(result_topic, "command/result");
    cJSON *obj = cJSON_CreateObject();
    if (request_id != NULL) cJSON_AddStringToObject(obj, "request_id", request_id);
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, "code", code != NULL ? code : (ok ? "accepted" : "error"));
    if (message_id != 0) cJSON_AddNumberToObject(obj, "message_id", message_id);
    if (replayed) cJSON_AddBoolToObject(obj, "replayed", true);
    publish_json_object(result_topic, obj, 1, 0);
    cJSON_Delete(obj);
}

static void handle_structured_send(const char *payload)
{
    portENTER_CRITICAL(&s_state_lock); ++s_diag.commands_received; portEXIT_CRITICAL(&s_state_lock);
    cJSON *json = cJSON_Parse(payload);
    const cJSON *request_id = json ? cJSON_GetObjectItemCaseSensitive(json, "request_id") : NULL;
    const cJSON *to = json ? cJSON_GetObjectItemCaseSensitive(json, "to") : NULL;
    const cJSON *text = json ? cJSON_GetObjectItemCaseSensitive(json, "text") : NULL;
    const cJSON *dr = json ? cJSON_GetObjectItemCaseSensitive(json, "delivery_report") : NULL;
    const bool delivery = dr == NULL ? true : cJSON_IsTrue(dr);
    if (!cJSON_IsString(request_id) || !gateway_idempotency_key_valid(request_id->valuestring) ||
        !cJSON_IsString(to) || !gateway_e164_valid(to->valuestring) || !cJSON_IsString(text) ||
        text->valuestring[0] == '\0' || strlen(text->valuestring) >= SMS_MESSAGE_TEXT_MAX ||
        (dr != NULL && !cJSON_IsBool(dr))) {
        portENTER_CRITICAL(&s_state_lock); ++s_diag.commands_rejected; portEXIT_CRITICAL(&s_state_lock);
        publish_command_result(cJSON_IsString(request_id) ? request_id->valuestring : NULL, false, "invalid_request", 0, false);
        cJSON_Delete(json); return;
    }
    sms_encoding_t preflight_encoding = SMS_ENCODING_UNKNOWN;
    size_t preflight_segments = 0;
    if (!sms_submit_preflight(to->valuestring, text->valuestring, 0x0100U, &preflight_encoding, &preflight_segments)) {
        portENTER_CRITICAL(&s_state_lock); ++s_diag.commands_rejected; portEXIT_CRITICAL(&s_state_lock);
        publish_command_result(request_id->valuestring, false, "message_too_long", 0, false);
        cJSON_Delete(json); return;
    }
    if (!gateway_rate_limiter_allow(&s_sms_limiter, 1.0, now_ms())) {
        publish_command_result(request_id->valuestring, false, "rate_limited", 0, false);
        cJSON_Delete(json); return;
    }
    uint8_t fingerprint[GATEWAY_SHA256_LEN];
    if (gateway_idempotency_sms_fingerprint(to->valuestring, text->valuestring, delivery, fingerprint) != ESP_OK) {
        publish_command_result(request_id->valuestring, false, "internal_error", 0, false); cJSON_Delete(json); return;
    }
    gateway_idempotency_result_t idem = GATEWAY_IDEMPOTENCY_MISS;
    uint32_t existing = 0;
    esp_err_t idem_err = gateway_idempotency_claim(request_id->valuestring, fingerprint, &idem, &existing);
    if (idem_err != ESP_OK) {
        publish_command_result(request_id->valuestring, false, "idempotency_unavailable", 0, false);
    } else if (idem == GATEWAY_IDEMPOTENCY_CONFLICT) {
        publish_command_result(request_id->valuestring, false, "idempotency_conflict", existing, false);
    } else if (idem == GATEWAY_IDEMPOTENCY_REPLAY) {
        if (existing == 0) {
            publish_command_result(request_id->valuestring, false, "idempotency_pending", 0, true);
        } else {
            sms_message_t *record = calloc(1, sizeof(*record));
            if (record != NULL && modem_core_sms_get(existing, record) == ESP_OK) publish_command_result(request_id->valuestring, true, "accepted", existing, true);
            else publish_command_result(request_id->valuestring, false, "idempotency_expired", existing, false);
            if (record) { gateway_security_wipe(record, sizeof(*record)); free(record); }
        }
    } else {
        uint32_t id = 0;
        esp_err_t send_err = modem_core_sms_send(to->valuestring, text->valuestring, delivery, &id);
        if (send_err != ESP_OK) {
            (void)gateway_idempotency_release_pending(request_id->valuestring, fingerprint);
            publish_command_result(request_id->valuestring, false, "queue_failed", 0, false);
        } else {
            bool finalized = false;
            for (unsigned attempt = 0; attempt < 3 && !finalized; ++attempt) {
                finalized = gateway_idempotency_finalize(request_id->valuestring, fingerprint, id) == ESP_OK;
            }
            publish_command_result(request_id->valuestring, true, finalized ? "accepted" : "accepted_idempotency_pending", id, false);
            if (!finalized) ESP_LOGE(TAG, "SMS id=%" PRIu32 " queued but MQTT idempotency finalization is pending", id);
        }
    }
    gateway_security_wipe(fingerprint, sizeof(fingerprint));
    cJSON_Delete(json);
}

static void handle_native_notify(const char *payload, size_t len)
{
    gateway_mqtt_config_t config = {0};
    sms_encoding_t encoding = SMS_ENCODING_UNKNOWN;
    size_t segments = 0;
    if (!config_snapshot(&config) || !config.default_recipient[0] || len == 0 || len >= SMS_MESSAGE_TEXT_MAX ||
        !sms_submit_preflight(config.default_recipient, payload, 0x0100U, &encoding, &segments) ||
        !gateway_rate_limiter_allow(&s_sms_limiter, 1.0, now_ms())) { gateway_security_wipe(&config, sizeof(config)); return; }
    uint32_t id = 0;
    (void)modem_core_sms_send(config.default_recipient, payload, true, &id);
    gateway_security_wipe(&config, sizeof(config));
}

static void reboot_task(void *arg) { (void)arg; vTaskDelay(pdMS_TO_TICKS(250)); esp_restart(); }

static void subscribe_and_announce(void)
{
    gateway_mqtt_config_t config = {0};
    if (!config_snapshot(&config)) return;
    if (s_client_mutex == NULL || xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) { gateway_security_wipe(&config, sizeof(config)); return; }
    esp_mqtt_client_handle_t client = s_client;
    if (client != NULL) {
        char t[MQTT_TOPIC_MAX];
        topic(t, "sms/send"); (void)esp_mqtt_client_subscribe(client, t, 1);
        topic(t, "modem/restart"); (void)esp_mqtt_client_subscribe(client, t, 1);
        topic(t, "system/reboot"); (void)esp_mqtt_client_subscribe(client, t, 1);
        if (config.default_recipient[0]) { topic(t, "ha/notify"); (void)esp_mqtt_client_subscribe(client, t, 0); }
        if (config.home_assistant_enabled) (void)esp_mqtt_client_subscribe(client, "homeassistant/status", 0);
    }
    xSemaphoreGive(s_client_mutex);
    char availability[MQTT_TOPIC_MAX]; topic(availability, "availability");
    (void)publish_raw(availability, "online", 1, 1);
    publish_discovery();
    publish_status();
    gateway_security_wipe(&config, sizeof(config));
}

static void handle_data_work(mqtt_work_t *work)
{
    char expected[MQTT_TOPIC_MAX];
    mqtt_command_kind_t kind = MQTT_COMMAND_NONE;

    topic(expected, "sms/send");
    if (strcmp(work->topic, expected) == 0) kind = MQTT_COMMAND_SMS_SEND;
    else {
        topic(expected, "ha/notify");
        if (strcmp(work->topic, expected) == 0) kind = MQTT_COMMAND_NATIVE_NOTIFY;
        else {
            topic(expected, "modem/restart");
            if (strcmp(work->topic, expected) == 0) kind = MQTT_COMMAND_MODEM_RESTART;
            else {
                topic(expected, "system/reboot");
                if (strcmp(work->topic, expected) == 0) kind = MQTT_COMMAND_SYSTEM_REBOOT;
                else if (strcmp(work->topic, "homeassistant/status") == 0) kind = MQTT_COMMAND_HA_STATUS;
            }
        }
    }

    if (kind == MQTT_COMMAND_NONE) return;
    if (!mqtt_command_allowed(kind, work->retained, work->payload)) {
        if (kind != MQTT_COMMAND_HA_STATUS) {
            portENTER_CRITICAL(&s_state_lock); ++s_diag.commands_rejected; portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGW(TAG, "rejected MQTT command kind=%d retained=%d", (int)kind, work->retained);
        }
        return;
    }

    switch (kind) {
    case MQTT_COMMAND_SMS_SEND:
        handle_structured_send(work->payload);
        break;
    case MQTT_COMMAND_NATIVE_NOTIFY:
        handle_native_notify(work->payload, work->payload_len);
        break;
    case MQTT_COMMAND_MODEM_RESTART:
        (void)modem_core_restart_modem();
        break;
    case MQTT_COMMAND_SYSTEM_REBOOT:
        (void)xTaskCreate(reboot_task, "mqtt_reboot", 2048, NULL, 3, NULL);
        break;
    case MQTT_COMMAND_HA_STATUS: {
        gateway_mqtt_config_t config = {0};
        const bool ha_enabled = config_snapshot(&config) && config.home_assistant_enabled;
        gateway_security_wipe(&config, sizeof(config));
        if (ha_enabled) { publish_discovery(); publish_status(); }
        break;
    }
    default:
        break;
    }
}

static esp_err_t replay_cursor_store(uint32_t message_id)
{
    esp_err_t err = nvs_set_u32(s_runtime_nvs, MQTT_REPLAY_CURSOR_KEY, message_id);
    if (err == ESP_OK) err = nvs_commit(s_runtime_nvs);
    if (err == ESP_OK) {
        s_replay_cursor = message_id;
        portENTER_CRITICAL(&s_state_lock); s_diag.replay_cursor = message_id; portEXIT_CRITICAL(&s_state_lock);
        modem_core_sms_set_event_replay_watermark(true, message_id);
    }
    return err;
}

static bool publish_inbound_replay(const sms_message_t *m)
{
    char out_topic[MQTT_TOPIC_MAX]; topic(out_topic, "sms/received");
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return false;
    cJSON_AddNumberToObject(obj, "id", m->id);
    cJSON_AddStringToObject(obj, "status", sms_message_status_name(m->status));
    cJSON_AddStringToObject(obj, "event_type", "received");
    cJSON_AddStringToObject(obj, "from", m->sender);
    cJSON_AddStringToObject(obj, "text", m->text);
    if (m->service_center_timestamp[0]) cJSON_AddStringToObject(obj, "received_at", m->service_center_timestamp);
    char *encoded = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (encoded == NULL) return false;
    const int packet_id = publish_raw(out_topic, encoded, 1, 0);
    gateway_security_wipe(encoded, strlen(encoded));
    cJSON_free(encoded);
    if (packet_id < 0) return false;
    portENTER_CRITICAL(&s_state_lock);
    s_replay_message_id = m->id;
    s_diag.replay_inflight_message_id = m->id;
    s_replay_packet_id = packet_id;
    s_replay_started_ms = now_ms();
    portEXIT_CRITICAL(&s_state_lock);
    return true;
}

static void replay_next_inbound(void)
{
    if (!connected()) return;
    portENTER_CRITICAL(&s_state_lock);
    const bool inflight = s_replay_message_id != 0;
    const uint32_t cursor = s_replay_cursor;
    portEXIT_CRITICAL(&s_state_lock);
    if (inflight) return;

    uint32_t scan = cursor;
    for (unsigned i = 0; i < SMS_STORE_MAX_RECORDS; ++i) {
        sms_message_t *record = calloc(1, sizeof(*record));
        if (record == NULL) return;
        size_t count = 0;
        esp_err_t err = modem_core_sms_list(scan, record, 1, &count);
        if (err != ESP_OK || count == 0) {
            gateway_security_wipe(record, sizeof(*record)); free(record); return;
        }
        scan = record->id;
        if (record->direction == SMS_DIRECTION_INBOUND && record->status == SMS_MESSAGE_RECEIVED) {
            const bool sent = publish_inbound_replay(record);
            gateway_security_wipe(record, sizeof(*record)); free(record);
            if (!sent) {
                portENTER_CRITICAL(&s_state_lock); ++s_diag.events_dropped; portEXIT_CRITICAL(&s_state_lock);
            }
            return;
        }
        gateway_security_wipe(record, sizeof(*record)); free(record);
    }
}

static void replay_inflight_clear(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_replay_message_id = 0;
    s_diag.replay_inflight_message_id = 0;
    s_replay_packet_id = 0;
    s_replay_started_ms = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static void handle_replay_ack(uint32_t message_id)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool current = s_replay_message_id == message_id && message_id != 0;
    if (current) {
        s_replay_message_id = 0;
        s_diag.replay_inflight_message_id = 0;
        s_replay_packet_id = 0;
        s_replay_started_ms = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!current) return;
    if (replay_cursor_store(message_id) != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist MQTT replay cursor for SMS id=%" PRIu32, message_id);
        return;
    }
    replay_next_inbound();
}

static void replay_watchdog(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool expired = s_replay_message_id != 0 &&
        s_replay_started_ms != 0 && now_ms() - s_replay_started_ms >= MQTT_REPLAY_ACK_TIMEOUT_MS;
    if (expired) {
        s_replay_message_id = 0;
        s_diag.replay_inflight_message_id = 0;
        s_replay_packet_id = 0;
        s_replay_started_ms = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (expired) {
        ESP_LOGW(TAG, "MQTT SMS event acknowledgement timed out; replaying at-least-once");
        replay_next_inbound();
    }
}

static void publish_sms_event(const mqtt_work_t *work)
{
    const mqtt_sms_event_snapshot_t *m = (const mqtt_sms_event_snapshot_t *)work->payload;
    char out_topic[MQTT_TOPIC_MAX];
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return;
    cJSON_AddNumberToObject(obj, "id", m->id);
    cJSON_AddStringToObject(obj, "status", sms_message_status_name(m->status));
    if (work->sms_event == SMS_SERVICE_EVENT_RECEIVED && m->direction == SMS_DIRECTION_INBOUND) {
        topic(out_topic, "sms/received");
        cJSON_AddStringToObject(obj, "event_type", "received");
        cJSON_AddStringToObject(obj, "from", m->sender);
        cJSON_AddStringToObject(obj, "text", m->text);
        if (m->service_center_timestamp[0]) cJSON_AddStringToObject(obj, "received_at", m->service_center_timestamp);
    } else {
        topic(out_topic, "sms/status");
        cJSON_AddStringToObject(obj, "direction", m->direction == SMS_DIRECTION_INBOUND ? "inbound" : "outbound");
    }
    publish_json_object(out_topic, obj, 1, 0);
    cJSON_Delete(obj);
    publish_status();
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        mqtt_work_t *work = NULL;
        if (xQueueReceive(s_queue, &work, pdMS_TO_TICKS(MQTT_STATUS_PERIOD_MS)) == pdTRUE && work != NULL) {
            if (xSemaphoreTake(s_runtime_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
                if (work->type == MQTT_WORK_CONNECTED) { subscribe_and_announce(); replay_next_inbound(); }
                else if (work->type == MQTT_WORK_DATA) handle_data_work(work);
                else if (work->type == MQTT_WORK_SMS_EVENT) publish_sms_event(work);
                else if (work->type == MQTT_WORK_REPLAY) replay_next_inbound();
                else if (work->type == MQTT_WORK_REPLAY_ACK) handle_replay_ack(work->message_id);
                xSemaphoreGive(s_runtime_mutex);
            } else {
                portENTER_CRITICAL(&s_state_lock); ++s_diag.events_dropped; portEXIT_CRITICAL(&s_state_lock);
            }
            gateway_security_wipe(work, sizeof(*work)); free(work);
        } else if (connected() && xSemaphoreTake(s_runtime_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            replay_watchdog();
            replay_next_inbound();
            publish_status();
            xSemaphoreGive(s_runtime_mutex);
        }
    }
}

static void queue_simple_work(mqtt_work_type_t type)
{
    mqtt_work_t *work = calloc(1, sizeof(*work));
    if (work == NULL) return;
    work->type = type;
    if (xQueueSend(s_queue, &work, 0) != pdTRUE) { free(work); portENTER_CRITICAL(&s_state_lock); ++s_diag.events_dropped; portEXIT_CRITICAL(&s_state_lock); }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event == NULL || event->client != active_client_get()) return;
    if (event_id == MQTT_EVENT_CONNECTED) {
        diag_connected(true); queue_simple_work(MQTT_WORK_CONNECTED); return;
    }
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        diag_connected(false);
        /* A QoS-1 publish may have reached the broker even when its PUBACK was lost.
         * Keep the durable cursor unchanged and clear only the attempt so reconnect
         * replays at-least-once immediately. */
        replay_inflight_clear();
        return;
    }
    if (event_id == MQTT_EVENT_ERROR) {
        portENTER_CRITICAL(&s_state_lock); s_diag.last_mqtt_error = event->error_handle != NULL ? event->error_handle->esp_tls_last_esp_err : ESP_FAIL; portEXIT_CRITICAL(&s_state_lock); return;
    }
    if (event_id == MQTT_EVENT_PUBLISHED) {
        uint32_t replay_id = 0;
        portENTER_CRITICAL(&s_state_lock);
        if (event->msg_id == s_replay_packet_id) replay_id = s_replay_message_id;
        portEXIT_CRITICAL(&s_state_lock);
        if (replay_id != 0 && s_queue != NULL) {
            mqtt_work_t *work = calloc(1, sizeof(*work));
            if (work != NULL) {
                work->type = MQTT_WORK_REPLAY_ACK;
                work->message_id = replay_id;
                if (xQueueSend(s_queue, &work, 0) != pdTRUE) { gateway_security_wipe(work, sizeof(*work)); free(work); }
            }
        }
        return;
    }
    if (event_id != MQTT_EVENT_DATA) return;
    if (event->total_data_len <= 0 || event->total_data_len > MQTT_PAYLOAD_MAX || event->current_data_offset < 0 || event->data_len < 0) return;
    if (event->current_data_offset == 0) {
        memset(&s_rx, 0, sizeof(s_rx));
        if (event->topic_len <= 0 || event->topic_len >= MQTT_TOPIC_MAX) return;
        memcpy(s_rx.topic, event->topic, (size_t)event->topic_len); s_rx.topic[event->topic_len] = 0;
        s_rx.total = (size_t)event->total_data_len;
        s_rx.retained = event->retain != 0;
        s_rx.duplicate = event->dup != 0;
        s_rx.qos = event->qos;
    }
    if ((size_t)event->current_data_offset != s_rx.received || s_rx.total != (size_t)event->total_data_len ||
        s_rx.received + (size_t)event->data_len > s_rx.total) { memset(&s_rx, 0, sizeof(s_rx)); return; }
    memcpy(s_rx.data + s_rx.received, event->data, (size_t)event->data_len);
    s_rx.received += (size_t)event->data_len;
    if (s_rx.received == s_rx.total) {
        mqtt_work_t *work = calloc(1, sizeof(*work));
        if (work != NULL) {
            work->type = MQTT_WORK_DATA; work->payload_len = s_rx.total;
            work->retained = s_rx.retained; work->duplicate = s_rx.duplicate; work->qos = s_rx.qos;
            memcpy(work->topic, s_rx.topic, sizeof(work->topic));
            memcpy(work->payload, s_rx.data, s_rx.total); work->payload[s_rx.total] = 0;
            if (xQueueSend(s_queue, &work, 0) != pdTRUE) { gateway_security_wipe(work, sizeof(*work)); free(work); }
        }
        gateway_security_wipe(&s_rx, sizeof(s_rx));
    }
}

static void sms_event_callback(sms_service_event_t event, const sms_message_t *message, void *ctx)
{
    (void)ctx;
    if (message == NULL || s_queue == NULL) return;
    if (event == SMS_SERVICE_EVENT_RECEIVED && message->direction == SMS_DIRECTION_INBOUND) {
        queue_simple_work(MQTT_WORK_REPLAY);
        return;
    }
    mqtt_work_t *work = calloc(1, sizeof(*work));
    if (work == NULL) return;
    mqtt_sms_event_snapshot_t snapshot = {0};
    snapshot.id = message->id;
    snapshot.direction = message->direction;
    snapshot.status = message->status;
    snprintf(snapshot.sender, sizeof(snapshot.sender), "%s", message->sender);
    snprintf(snapshot.recipient, sizeof(snapshot.recipient), "%s", message->recipient);
    snprintf(snapshot.service_center_timestamp, sizeof(snapshot.service_center_timestamp), "%s", message->service_center_timestamp);
    snprintf(snapshot.text, sizeof(snapshot.text), "%s", message->text);
    work->type = MQTT_WORK_SMS_EVENT;
    work->sms_event = event;
    memcpy(work->payload, &snapshot, sizeof(snapshot));
    work->payload_len = sizeof(snapshot);
    gateway_security_wipe(&snapshot, sizeof(snapshot));
    if (xQueueSend(s_queue, &work, 0) != pdTRUE) {
        gateway_security_wipe(work, sizeof(*work));
        free(work);
        portENTER_CRITICAL(&s_state_lock); ++s_diag.events_dropped; portEXIT_CRITICAL(&s_state_lock);
    }
}

esp_err_t mqtt_service_reconfigure(void)
{
    gateway_mqtt_config_t next = {0};
    esp_err_t err = gateway_settings_get_mqtt(&next);
    if (err != ESP_OK) return err;
    if (gateway_mqtt_config_validate(&next) != ESP_OK) {
        gateway_security_wipe(&next, sizeof(next));
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * ESP-MQTT does NOT copy broker.verification.certificate. Allocate the
     * next certificate before touching the active runtime, then transfer
     * ownership to the new client only after its config has been installed.
     */
    char *next_ca_pem = NULL;
    if (next.enabled && gateway_mqtt_uri_is_tls(next.broker_uri) && next.ca_pem[0] != '\0') {
        next_ca_pem = owned_ca_duplicate(next.ca_pem);
        if (next_ca_pem == NULL) {
            gateway_security_wipe(&next, sizeof(next));
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_runtime_mutex == NULL || xSemaphoreTake(s_runtime_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        return ESP_ERR_TIMEOUT;
    }
    if (s_client_mutex == NULL || xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        xSemaphoreGive(s_runtime_mutex);
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        return ESP_ERR_TIMEOUT;
    }
    if (s_config_mutex == NULL || xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xSemaphoreGive(s_client_mutex);
        xSemaphoreGive(s_runtime_mutex);
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        return ESP_ERR_TIMEOUT;
    }

    gateway_mqtt_config_t previous = s_config;
    xSemaphoreGive(s_config_mutex);

    esp_mqtt_client_handle_t old = active_client_get();
    if (old != NULL && connected()) {
        char old_topic[MQTT_TOPIC_MAX];
        snprintf(old_topic, sizeof(old_topic), "%s/availability", previous.base_topic);
        (void)esp_mqtt_client_publish(old, old_topic, "offline", 0, 1, 1);
        snprintf(old_topic, sizeof(old_topic), "%s/status", previous.base_topic);
        (void)esp_mqtt_client_publish(old, old_topic, "", 0, 1, 1);
        if (previous.home_assistant_enabled) {
            char discovery_topic[MQTT_TOPIC_MAX];
            snprintf(discovery_topic, sizeof(discovery_topic), "%s/device/%s/config",
                     previous.discovery_prefix, network_service_device_id());
            (void)esp_mqtt_client_publish(old, discovery_topic, "", 0, 1, 1);
        }
    }

    /*
     * Make callbacks from the retiring client stale before stopping it. The CA
     * allocation must remain valid through stop+destroy because TLS teardown can
     * still consult client configuration.
     */
    active_client_set(NULL);
    diag_connected(false);
    if (old != NULL) {
        (void)esp_mqtt_client_stop(old);
        (void)esp_mqtt_client_destroy(old);
    }
    owned_ca_free(&s_active_ca_pem);

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        gateway_security_wipe(&previous, sizeof(previous));
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        xSemaphoreGive(s_client_mutex);
        xSemaphoreGive(s_runtime_mutex);
        return ESP_ERR_TIMEOUT;
    }
    gateway_security_wipe(&s_config, sizeof(s_config));
    s_config = next;
    xSemaphoreGive(s_config_mutex);
    modem_core_sms_set_event_replay_watermark(s_config.enabled, s_config.enabled ? s_replay_cursor : UINT32_MAX);
    portENTER_CRITICAL(&s_state_lock);
    s_diag.enabled = s_config.enabled;
    portEXIT_CRITICAL(&s_state_lock);
    gateway_security_wipe(&previous, sizeof(previous));

    if (!s_config.enabled) {
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        xSemaphoreGive(s_client_mutex);
        xSemaphoreGive(s_runtime_mutex);
        return ESP_OK;
    }

    char availability[MQTT_TOPIC_MAX];
    topic(availability, "availability");
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = s_config.username[0] ? s_config.username : NULL,
        .credentials.client_id = network_service_device_id(),
        .credentials.authentication.password = s_config.password[0] ? s_config.password : NULL,
        .session.last_will.topic = availability,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 60,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .task.stack_size = 6144,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .outbox.limit = 32768,
    };
    if (gateway_mqtt_uri_is_tls(s_config.broker_uri)) {
        if (next_ca_pem != NULL) {
            cfg.broker.verification.certificate = next_ca_pem;
            cfg.broker.verification.certificate_len = 0; /* PEM, NUL terminated */
        } else {
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    esp_mqtt_client_handle_t new_client = esp_mqtt_client_init(&cfg);
    if (new_client == NULL) {
        owned_ca_free(&next_ca_pem);
        gateway_security_wipe(&next, sizeof(next));
        xSemaphoreGive(s_client_mutex);
        xSemaphoreGive(s_runtime_mutex);
        return ESP_FAIL;
    }

    /* Transfer certificate ownership before start: CONNECTED/ERROR events may
     * arrive immediately after esp_mqtt_client_start(). */
    s_active_ca_pem = next_ca_pem;
    next_ca_pem = NULL;
    active_client_set(new_client);
    (void)esp_mqtt_client_register_event(new_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    err = esp_mqtt_client_start(new_client);
    if (err != ESP_OK) {
        active_client_set(NULL);
        (void)esp_mqtt_client_destroy(new_client);
        owned_ca_free(&s_active_ca_pem);
    }

    gateway_security_wipe(&next, sizeof(next));
    xSemaphoreGive(s_client_mutex);
    xSemaphoreGive(s_runtime_mutex);
    return err;
}

esp_err_t mqtt_service_init(void)
{
    if (s_diag.initialized) return ESP_ERR_INVALID_STATE;
    s_queue = xQueueCreate(MQTT_WORK_QUEUE_DEPTH, sizeof(mqtt_work_t *));
    s_client_mutex = xSemaphoreCreateMutex();
    s_config_mutex = xSemaphoreCreateMutex();
    s_runtime_mutex = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_client_mutex == NULL || s_config_mutex == NULL || s_runtime_mutex == NULL) return ESP_ERR_NO_MEM;
    gateway_rate_limiter_init(&s_sms_limiter, 5.0, 1.0 / 6.0, now_ms());
    esp_err_t nvs_err = nvs_open(MQTT_REPLAY_NAMESPACE, NVS_READWRITE, &s_runtime_nvs);
    if (nvs_err != ESP_OK) return nvs_err;
    uint32_t cursor = 0;
    nvs_err = nvs_get_u32(s_runtime_nvs, MQTT_REPLAY_CURSOR_KEY, &cursor);
    if (nvs_err != ESP_OK && nvs_err != ESP_ERR_NVS_NOT_FOUND) return nvs_err;
    s_replay_cursor = nvs_err == ESP_OK ? cursor : 0;
    portENTER_CRITICAL(&s_state_lock); s_diag.replay_cursor = s_replay_cursor; portEXIT_CRITICAL(&s_state_lock);
    modem_core_sms_set_event_replay_watermark(true, s_replay_cursor);
    if (xTaskCreate(worker_task, "mqtt_worker", MQTT_WORKER_STACK, NULL, 7, &s_worker) != pdPASS) return ESP_ERR_NO_MEM;
    (void)modem_core_set_sms_event_callback(sms_event_callback, NULL);
    portENTER_CRITICAL(&s_state_lock); s_diag.initialized = true; portEXIT_CRITICAL(&s_state_lock);
    return mqtt_service_reconfigure();
}

esp_err_t mqtt_service_get_diagnostics(mqtt_service_diagnostics_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock); *out = s_diag; portEXIT_CRITICAL(&s_state_lock);
    return s_diag.initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}
