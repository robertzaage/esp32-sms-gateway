#include "api_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_common.h"
#include "api_idempotency.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "modem_recovery_policy.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_security.h"
#include "gateway_board.h"
#include "gateway_settings.h"
#include "mqtt_service.h"
#include "modem_core.h"
#include "network_service.h"
#include "ota_service.h"
#include "sms_codec.h"

#define API_BODY_MAX 4608
#define API_AUTH_MAX 160
#define API_QUERY_MAX 256
#define API_LIST_MAX 25

static const char *TAG = "api";
static httpd_handle_t s_server;
static gateway_rate_limiter_t s_request_limiter;
static gateway_rate_limiter_t s_sms_limiter;
static gateway_rate_limiter_t s_ota_limiter;

static void reboot_task(void *arg);

static const char *recovery_action_name(modem_recovery_action_t action)
{
    switch (action) {
    case MODEM_RECOVERY_REINITIALIZE: return "reinitialize";
    case MODEM_RECOVERY_FUNCTIONAL_RESET: return "functional_reset";
    case MODEM_RECOVERY_HARD_RESET: return "hard_reset";
    case MODEM_RECOVERY_NONE:
    default: return "none";
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void add_json_string(cJSON *obj, const char *name, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        cJSON_AddStringToObject(obj, name, value);
    } else {
        cJSON_AddNullToObject(obj, name);
    }
}

static bool json_has_only_fields(const cJSON *json, const char *const *allowed, size_t allowed_count)
{
    if (!cJSON_IsObject(json)) return false;
    for (const cJSON *item = json->child; item != NULL; item = item->next) {
        if (item->string == NULL) return false;
        bool found = false;
        for (size_t i = 0; i < allowed_count; ++i) {
            if (strcmp(item->string, allowed[i]) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

static cJSON *message_json(const sms_message_t *message)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "id", message->id);
    cJSON_AddStringToObject(obj, "direction", message->direction == SMS_DIRECTION_INBOUND ? "inbound" : "outbound");
    cJSON_AddStringToObject(obj, "status", sms_message_status_name(message->status));
    if (message->direction == SMS_DIRECTION_INBOUND) {
        add_json_string(obj, "from", message->sender);
        cJSON_AddNullToObject(obj, "to");
    } else {
        cJSON_AddNullToObject(obj, "from");
        add_json_string(obj, "to", message->recipient);
    }
    cJSON_AddStringToObject(obj, "text", message->status == SMS_MESSAGE_UNSUPPORTED ? "" : message->text);
    add_json_string(obj, "service_center_timestamp", message->service_center_timestamp);
    cJSON_AddBoolToObject(obj, "delivery_report_requested", message->delivery_report_requested);
    cJSON_AddNumberToObject(obj, "segment_count", message->segment_count);
    cJSON_AddNumberToObject(obj, "send_attempts", message->send_attempts);
    if (message->last_modem_error >= 0) {
        cJSON_AddNumberToObject(obj, "last_modem_error", message->last_modem_error);
    } else {
        cJSON_AddNullToObject(obj, "last_modem_error");
    }
    return obj;
}

static esp_err_t send_json(httpd_req_t *req, int status, cJSON *json)
{
    char status_line[32];
    snprintf(status_line, sizeof(status_line), "%d %s", status,
             status == 200 ? "OK" : status == 202 ? "Accepted" : "Response");
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    char *encoded = cJSON_PrintUnformatted(json);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = httpd_resp_send(req, encoded, HTTPD_RESP_USE_STRLEN) == ESP_OK ? ESP_OK : ESP_FAIL;
    gateway_security_wipe(encoded, strlen(encoded));
    cJSON_free(encoded);
    return err;
}

static esp_err_t problem(httpd_req_t *req, int status, const char *type, const char *title, const char *detail)
{
    char status_line[40];
    snprintf(status_line, sizeof(status_line), "%d %s", status, title);
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/problem+json");
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "type", type);
    cJSON_AddStringToObject(obj, "title", title);
    cJSON_AddNumberToObject(obj, "status", status);
    if (detail != NULL) {
        cJSON_AddStringToObject(obj, "detail", detail);
    }
    char *encoded = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = httpd_resp_send(req, encoded, HTTPD_RESP_USE_STRLEN) == ESP_OK ? ESP_OK : ESP_FAIL;
    gateway_security_wipe(encoded, strlen(encoded));
    cJSON_free(encoded);
    return result;
}

static bool request_allowed(httpd_req_t *req, bool sms)
{
    if (!gateway_rate_limiter_allow(&s_request_limiter, 1.0, now_ms())) {
        (void)problem(req, 429, "urn:sms-gateway:rate-limit", "Too Many Requests", "API request rate exceeded");
        return false;
    }
    if (sms && !gateway_rate_limiter_allow(&s_sms_limiter, 1.0, now_ms())) {
        (void)problem(req, 429, "urn:sms-gateway:sms-rate-limit", "Too Many Requests", "Outbound SMS rate exceeded");
        return false;
    }
    return true;
}

static bool authorized(httpd_req_t *req)
{
    char header[API_AUTH_MAX];
    const size_t length = httpd_req_get_hdr_value_len(req, "Authorization");
    if (length < 8 || length >= sizeof(header) || httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        (void)problem(req, 401, "urn:sms-gateway:unauthorized", "Unauthorized", "Bearer token required");
        return false;
    }
    const char prefix[] = "Bearer ";
    const bool ok = strncmp(header, prefix, sizeof(prefix) - 1) == 0 &&
                    gateway_security_validate_bearer(header + sizeof(prefix) - 1);
    gateway_security_wipe(header, sizeof(header));
    if (!ok) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        (void)problem(req, 401, "urn:sms-gateway:unauthorized", "Unauthorized", "Invalid bearer token");
    }
    return ok;
}

static esp_err_t read_body(httpd_req_t *req, char **out)
{
    *out = NULL;
    if (req->content_len <= 0 || req->content_len > API_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = calloc(1, (size_t)req->content_len + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int n = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (n <= 0) {
            gateway_security_wipe(body, (size_t)req->content_len + 1);
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    *out = body;
    return ESP_OK;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    const esp_err_t err = send_json(req, 200, obj);
    cJSON_Delete(obj);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    modem_manager_snapshot_t modem = {0};
    modem_usb_diagnostics_t usb = {0};
    sms_service_diagnostics_t sms = {0};
    network_service_snapshot_t network = {0};
    (void)modem_core_manager_snapshot(&modem);
    (void)modem_core_usb_diagnostics(&usb);
    (void)modem_core_sms_diagnostics(&sms);
    (void)network_service_get_snapshot(&network);

    cJSON *root = cJSON_CreateObject();
    cJSON *gateway = cJSON_AddObjectToObject(root, "gateway");
    cJSON_AddStringToObject(gateway, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(gateway, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(gateway, "device_id", network.device_id);
    cJSON_AddStringToObject(gateway, "ipv4", network.ipv4);
    cJSON_AddBoolToObject(gateway, "wifi_connected", network.connected);
    cJSON_AddBoolToObject(gateway, "time_synced", network.time_synced);

    cJSON *m = cJSON_AddObjectToObject(root, "modem");
    cJSON_AddStringToObject(m, "state", modem_core_state_name(modem_core_state()));
    cJSON_AddBoolToObject(m, "registered", modem.registered);
    cJSON_AddBoolToObject(m, "roaming", modem.roaming);
    cJSON_AddStringToObject(m, "sim", modem_sim_state_name(modem.sim));
    add_json_string(m, "model", modem.model);
    add_json_string(m, "revision", modem.revision);
    add_json_string(m, "operator", modem.operator_info.name);
    if (modem.signal.rssi == 99) cJSON_AddNullToObject(m, "rssi_dbm");
    else cJSON_AddNumberToObject(m, "rssi_dbm", modem.signal.rssi_dbm);
    cJSON_AddNumberToObject(m, "usb_reconnects", usb.disconnect_count);
    cJSON_AddNumberToObject(m, "usb_mode_switch_attempts", usb.mode_switch_attempts);
    cJSON_AddNumberToObject(m, "usb_mode_switch_successes", usb.mode_switch_successes);
    cJSON_AddNumberToObject(m, "usb_mode_switch_failures", usb.mode_switch_failures);
    gateway_board_power_diagnostics_t power = {0};
    if (gateway_board_power_diagnostics(&power) == ESP_OK) {
        cJSON_AddBoolToObject(m, "usb_overcurrent", power.overcurrent);
        cJSON_AddBoolToObject(m, "usb_power_cutoff_latched", power.cutoff_latched);
        cJSON_AddNumberToObject(m, "usb_overcurrent_events", power.overcurrent_events);
        cJSON_AddNumberToObject(m, "usb_power_cutoffs", power.power_cutoffs);
    }
    cJSON_AddNumberToObject(m, "recovery_count", modem.recovery_attempts);
    cJSON_AddStringToObject(m, "last_recovery_action", recovery_action_name(modem.last_recovery_action));

    cJSON *s = cJSON_AddObjectToObject(root, "sms");
    cJSON_AddNumberToObject(s, "outbound_queued", sms.outbound_queued);
    cJSON_AddNumberToObject(s, "inbound_messages", sms.inbound_messages);
    cJSON_AddNumberToObject(s, "delivery_reports", sms.delivery_reports);
    cJSON_AddNumberToObject(s, "uncertain", sms.outbound_uncertain);
    cJSON_AddNumberToObject(s, "store_used", sms.store_used_records);
    cJSON_AddNumberToObject(s, "store_free", sms.store_free_records);
    cJSON_AddNumberToObject(s, "store_pruned", sms.store_pruned_records);
    cJSON_AddNumberToObject(s, "store_capacity_failures", sms.store_capacity_failures);
    gateway_idempotency_diagnostics_t idem = {0};
    if (gateway_idempotency_get_diagnostics(&idem) == ESP_OK) {
        cJSON *idem_obj = cJSON_AddObjectToObject(root, "idempotency");
        cJSON_AddNumberToObject(idem_obj, "pending", idem.pending_records);
        cJSON_AddNumberToObject(idem_obj, "finalized", idem.finalized_records);
        cJSON_AddNumberToObject(idem_obj, "free", idem.free_records);
    }

    gateway_ota_diagnostics_t ota = {0};
    cJSON *ot = cJSON_AddObjectToObject(root, "ota");
    if (gateway_ota_get_diagnostics(&ota) == ESP_OK) {
        cJSON_AddBoolToObject(ot, "pending_verify", ota.pending_verify);
        cJSON_AddBoolToObject(ot, "confirmation_scheduled", ota.confirmation_scheduled);
        cJSON_AddBoolToObject(ot, "update_in_progress", ota.update_in_progress);
        cJSON_AddNumberToObject(ot, "upload_attempts", ota.upload_attempts);
        cJSON_AddNumberToObject(ot, "upload_successes", ota.upload_successes);
        cJSON_AddNumberToObject(ot, "upload_rejections", ota.upload_rejections);
        cJSON_AddNumberToObject(ot, "upload_failures", ota.upload_failures);
        cJSON_AddNumberToObject(ot, "last_error", ota.last_error);
    }

    mqtt_service_diagnostics_t mqtt = {0};
    cJSON *mq = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt_service_get_diagnostics(&mqtt) == ESP_OK) {
        cJSON_AddBoolToObject(mq, "enabled", mqtt.enabled);
        cJSON_AddBoolToObject(mq, "connected", mqtt.connected);
        cJSON_AddNumberToObject(mq, "connects", mqtt.connects);
        cJSON_AddNumberToObject(mq, "disconnects", mqtt.disconnects);
        cJSON_AddNumberToObject(mq, "events_dropped", mqtt.events_dropped);
        cJSON_AddNumberToObject(mq, "replay_cursor", mqtt.replay_cursor);
        cJSON_AddNumberToObject(mq, "replay_inflight_message_id", mqtt.replay_inflight_message_id);
        cJSON_AddNumberToObject(mq, "last_error", mqtt.last_mqtt_error);
    } else {
        cJSON_AddBoolToObject(mq, "enabled", false);
        cJSON_AddBoolToObject(mq, "connected", false);
    }
    const esp_err_t err = send_json(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static bool header_is_true(httpd_req_t *req, const char *name)
{
    char value[8] = {0};
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    return len == 4 && httpd_req_get_hdr_value_str(req, name, value, sizeof(value)) == ESP_OK && strcmp(value, "true") == 0;
}

static esp_err_t firmware_get_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    gateway_ota_diagnostics_t ota = {0};
    if (gateway_ota_get_diagnostics(&ota) != ESP_OK) {
        return problem(req, 503, "urn:sms-gateway:ota-unavailable", "Service Unavailable", "OTA diagnostics are unavailable");
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(obj, "running_version", ota.running_version);
    cJSON_AddStringToObject(obj, "running_partition", ota.running_partition);
    cJSON_AddStringToObject(obj, "boot_version", ota.boot_version);
    cJSON_AddStringToObject(obj, "boot_partition", ota.boot_partition);
    cJSON_AddBoolToObject(obj, "pending_verify", ota.pending_verify);
    cJSON_AddBoolToObject(obj, "confirmation_scheduled", ota.confirmation_scheduled);
    cJSON_AddBoolToObject(obj, "update_in_progress", ota.update_in_progress);
    cJSON_AddNumberToObject(obj, "upload_attempts", ota.upload_attempts);
    cJSON_AddNumberToObject(obj, "upload_successes", ota.upload_successes);
    cJSON_AddNumberToObject(obj, "upload_rejections", ota.upload_rejections);
    cJSON_AddNumberToObject(obj, "upload_failures", ota.upload_failures);
    cJSON_AddNumberToObject(obj, "last_error", ota.last_error);
    add_json_string(obj, "last_candidate_version", ota.last_candidate_version);
    const esp_err_t err = send_json(req, 200, obj);
    cJSON_Delete(obj);
    return err;
}

static esp_err_t ota_problem(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case GATEWAY_OTA_ERR_HASH_MISMATCH:
        return problem(req, 422, "urn:sms-gateway:ota-hash", "Unprocessable Content", "Firmware SHA-256 does not match X-Firmware-SHA256");
    case GATEWAY_OTA_ERR_PROJECT_MISMATCH:
        return problem(req, 422, "urn:sms-gateway:ota-project", "Unprocessable Content", "Firmware belongs to a different project");
    case GATEWAY_OTA_ERR_VERSION_FORMAT:
        return problem(req, 422, "urn:sms-gateway:ota-version", "Unprocessable Content", "Firmware version is not valid semantic version syntax");
    case GATEWAY_OTA_ERR_REINSTALL_REJECTED:
        return problem(req, 409, "urn:sms-gateway:ota-reinstall", "Conflict", "Reinstalling the currently running version requires X-Firmware-Allow-Reinstall: true");
    case GATEWAY_OTA_ERR_DOWNGRADE_REJECTED:
        return problem(req, 409, "urn:sms-gateway:ota-downgrade", "Conflict", "Firmware downgrade requires X-Firmware-Allow-Downgrade: true");
    case GATEWAY_OTA_ERR_SECURE_VERSION:
        return problem(req, 422, "urn:sms-gateway:ota-secure-version", "Unprocessable Content", "Firmware secure_version is lower than the running image");
    case GATEWAY_OTA_ERR_PENDING_VERIFY:
        return problem(req, 409, "urn:sms-gateway:ota-pending-verify", "Conflict", "The running OTA image has not completed its rollback confirmation window");
    case GATEWAY_OTA_ERR_IMAGE_SIZE:
        return problem(req, 413, "urn:sms-gateway:ota-size", "Content Too Large", "Firmware image does not fit the inactive OTA partition or is truncated");
    case ESP_ERR_INVALID_STATE:
        return problem(req, 409, "urn:sms-gateway:ota-busy", "Conflict", "Another firmware update is already in progress");
    case ESP_ERR_INVALID_ARG:
        return problem(req, 400, "urn:sms-gateway:ota-request", "Bad Request", "Invalid firmware upload metadata");
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return problem(req, 422, "urn:sms-gateway:ota-image", "Unprocessable Content", "ESP-IDF rejected the application image");
    default:
        return problem(req, 503, "urn:sms-gateway:ota-failed", "Service Unavailable", "Firmware update could not be staged safely");
    }
}

static esp_err_t firmware_post_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    if (!gateway_rate_limiter_allow(&s_ota_limiter, 1.0, now_ms())) {
        return problem(req, 429, "urn:sms-gateway:ota-rate-limit", "Too Many Requests", "Firmware upload rate exceeded");
    }
    if (req->content_len <= 0) {
        return problem(req, 400, "urn:sms-gateway:ota-body", "Bad Request", "Firmware body is required");
    }

    char content_type[48] = {0};
    const size_t content_type_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (content_type_len == 0 || content_type_len >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/octet-stream") != 0) {
        return problem(req, 415, "urn:sms-gateway:ota-content-type", "Unsupported Media Type", "Use application/octet-stream for OTA images");
    }

    char sha256[GATEWAY_OTA_SHA256_HEX_LEN + 1] = {0};
    const size_t sha_len = httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256");
    if (sha_len != GATEWAY_OTA_SHA256_HEX_LEN ||
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", sha256, sizeof(sha256)) != ESP_OK) {
        return problem(req, 400, "urn:sms-gateway:ota-sha256", "Bad Request", "X-Firmware-SHA256 must contain the 64-character image SHA-256");
    }

    gateway_ota_request_t request = {
        .image_size = (size_t)req->content_len,
        .sha256_hex = sha256,
        .allow_reinstall = header_is_true(req, "X-Firmware-Allow-Reinstall"),
        .allow_downgrade = header_is_true(req, "X-Firmware-Allow-Downgrade"),
    };
    gateway_ota_session_t *session = NULL;
    esp_err_t err = gateway_ota_begin(&request, &session);
    gateway_security_wipe(sha256, sizeof(sha256));
    if (err != ESP_OK) return ota_problem(req, err);

    uint8_t *chunk = malloc(4096);
    if (chunk == NULL) {
        gateway_ota_abort(session);
        return problem(req, 503, "urn:sms-gateway:ota-memory", "Service Unavailable", "OTA receive buffer could not be allocated");
    }
    size_t received = 0;
    unsigned consecutive_timeouts = 0;
    while (received < (size_t)req->content_len) {
        const size_t remaining = (size_t)req->content_len - received;
        const size_t wanted = remaining < 4096 ? remaining : 4096;
        const int n = httpd_req_recv(req, (char *)chunk, wanted);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && consecutive_timeouts++ < 3) continue;
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        consecutive_timeouts = 0;
        err = gateway_ota_write(session, chunk, (size_t)n);
        if (err != ESP_OK) break;
        received += (size_t)n;
    }
    memset(chunk, 0, 4096);
    free(chunk);
    if (err != ESP_OK) {
        gateway_ota_abort(session);
        return ota_problem(req, err);
    }

    gateway_ota_result_t result = {0};
    err = gateway_ota_finish(session, &result);
    session = NULL;
    if (err != ESP_OK) return ota_problem(req, err);

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(obj, "status", "staged");
    cJSON_AddStringToObject(obj, "version", result.version);
    cJSON_AddStringToObject(obj, "project", result.project_name);
    cJSON_AddStringToObject(obj, "partition", result.partition);
    cJSON_AddStringToObject(obj, "sha256", result.sha256);
    cJSON_AddNumberToObject(obj, "size", result.image_size);
    cJSON_AddBoolToObject(obj, "rebooting", true);
    err = send_json(req, 202, obj);
    cJSON_Delete(obj);
    if (err == ESP_OK) {
        (void)xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 3, NULL);
    }
    memset(&result, 0, sizeof(result));
    return err;
}

static bool parse_message_id(const char *uri, uint32_t *id, bool *retry)
{
    const char *prefix = "/api/v1/messages/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) return false;
    const char *p = uri + strlen(prefix);
    char *end = NULL;
    const unsigned long value = strtoul(p, &end, 10);
    if (value == 0 || value > UINT32_MAX || end == p) return false;
    *retry = strcmp(end, "/retry") == 0;
    if (*end != '\0' && !*retry) return false;
    *id = (uint32_t)value;
    return true;
}

static esp_err_t send_message_record(httpd_req_t *req, int status, const sms_message_t *message)
{
    cJSON *obj = message_json(message);
    if (obj == NULL) return ESP_ERR_NO_MEM;
    const esp_err_t err = send_json(req, status, obj);
    cJSON_Delete(obj);
    return err;
}

static esp_err_t messages_get_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    char query[API_QUERY_MAX] = {0};
    char value[32];
    uint32_t after_id = 0;
    unsigned limit = 10;
    int direction = -1;
    if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "after_id", value, sizeof(value)) == ESP_OK) after_id = (uint32_t)strtoul(value, NULL, 10);
        if (httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) {
            limit = (unsigned)strtoul(value, NULL, 10);
            if (limit < 1 || limit > API_LIST_MAX) return problem(req, 400, "urn:sms-gateway:invalid-limit", "Bad Request", "limit must be 1..25");
        }
        if (httpd_query_key_value(query, "direction", value, sizeof(value)) == ESP_OK) {
            if (strcmp(value, "inbound") == 0) direction = SMS_DIRECTION_INBOUND;
            else if (strcmp(value, "outbound") == 0) direction = SMS_DIRECTION_OUTBOUND;
            else return problem(req, 400, "urn:sms-gateway:invalid-direction", "Bad Request", "direction must be inbound or outbound");
        }
    }
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, "{\"messages\":[", HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    bool first = true;
    uint32_t cursor = after_id;
    unsigned emitted = 0;
    while (emitted < limit) {
        sms_message_t *message = calloc(1, sizeof(*message));
        if (message == NULL) return ESP_ERR_NO_MEM;
        size_t count = 0;
        const esp_err_t err = modem_core_sms_list(cursor, message, 1, &count);
        if (err != ESP_OK || count == 0) {
            gateway_security_wipe(message, sizeof(*message));
            free(message);
            break;
        }
        cursor = message->id;
        if (direction < 0 || (int)message->direction == direction) {
            cJSON *obj = message_json(message);
            char *encoded = obj != NULL ? cJSON_PrintUnformatted(obj) : NULL;
            cJSON_Delete(obj);
            if (encoded == NULL) {
                gateway_security_wipe(message, sizeof(*message));
                free(message);
                return ESP_ERR_NO_MEM;
            }
            if (!first) (void)httpd_resp_send_chunk(req, ",", 1);
            first = false;
            (void)httpd_resp_send_chunk(req, encoded, HTTPD_RESP_USE_STRLEN);
            gateway_security_wipe(encoded, strlen(encoded));
            cJSON_free(encoded);
            ++emitted;
        }
        gateway_security_wipe(message, sizeof(*message));
        free(message);
    }
    char tail[64];
    snprintf(tail, sizeof(tail), "],\"next_after_id\":%" PRIu32 "}", cursor);
    (void)httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t messages_post_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, true)) return ESP_OK;
    char *body = NULL;
    esp_err_t err = read_body(req, &body);
    if (err != ESP_OK) return problem(req, 400, "urn:sms-gateway:invalid-body", "Bad Request", "Request body is missing or too large");
    cJSON *json = cJSON_Parse(body);
    static const char *const allowed[] = {"to", "text", "request_delivery_report"};
    if (json == NULL || !json_has_only_fields(json, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
        return problem(req, 400, "urn:sms-gateway:invalid-json", "Bad Request", "Invalid JSON body or unknown field");
    }
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(json, "to");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
    const cJSON *dr = cJSON_GetObjectItemCaseSensitive(json, "request_delivery_report");
    const bool delivery = dr == NULL ? true : cJSON_IsTrue(dr);
    if (!cJSON_IsString(to) || !cJSON_IsString(text) || !gateway_e164_valid(to->valuestring) ||
        text->valuestring[0] == '\0' || strlen(text->valuestring) >= SMS_MESSAGE_TEXT_MAX ||
        (dr != NULL && !cJSON_IsBool(dr))) {
        cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
        return problem(req, 400, "urn:sms-gateway:invalid-message", "Bad Request", "Invalid E.164 recipient, text, or delivery-report flag");
    }
    sms_encoding_t preflight_encoding = SMS_ENCODING_UNKNOWN;
    size_t preflight_segments = 0;
    if (!sms_submit_preflight(to->valuestring, text->valuestring, 0x0100U, &preflight_encoding, &preflight_segments)) {
        cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
        return problem(req, 422, "urn:sms-gateway:message-too-long", "Unprocessable Content", "Message cannot be encoded within the 16-segment SMS limit");
    }

    char idem_key[129] = {0};
    const size_t idem_len = httpd_req_get_hdr_value_len(req, "Idempotency-Key");
    const bool has_idem = idem_len > 0;
    bool idem_claimed = false;
    bool idem_finalized = false;
    uint8_t fingerprint[GATEWAY_SHA256_LEN] = {0};
    if (has_idem) {
        if (idem_len >= sizeof(idem_key) || httpd_req_get_hdr_value_str(req, "Idempotency-Key", idem_key, sizeof(idem_key)) != ESP_OK ||
            !gateway_idempotency_key_valid(idem_key) ||
            gateway_idempotency_sms_fingerprint(to->valuestring, text->valuestring, delivery, fingerprint) != ESP_OK) {
            cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
            return problem(req, 400, "urn:sms-gateway:invalid-idempotency-key", "Bad Request", "Invalid Idempotency-Key");
        }
        gateway_idempotency_result_t replay = GATEWAY_IDEMPOTENCY_MISS;
        uint32_t existing_id = 0;
        err = gateway_idempotency_claim(idem_key, fingerprint, &replay, &existing_id);
        if (err != ESP_OK) {
            cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
            gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
            return problem(req, 503, "urn:sms-gateway:idempotency-unavailable", "Service Unavailable", "Idempotency reservation could not be persisted; SMS was not queued");
        }
        if (replay == GATEWAY_IDEMPOTENCY_CONFLICT) {
            cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
            gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
            return problem(req, 409, "urn:sms-gateway:idempotency-conflict", "Conflict", "Idempotency-Key was already used with a different request");
        }
        if (replay == GATEWAY_IDEMPOTENCY_REPLAY) {
            cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
            if (existing_id == 0) {
                gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
                return problem(req, 409, "urn:sms-gateway:idempotency-pending", "Conflict", "A previous request with this key is durably reserved but its message ID is not finalized; using another key could duplicate an SMS");
            }
            sms_message_t *existing = calloc(1, sizeof(*existing));
            if (existing == NULL) {
                gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
                return ESP_ERR_NO_MEM;
            }
            err = modem_core_sms_get(existing_id, existing);
            gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
            if (err != ESP_OK) {
                gateway_security_wipe(existing, sizeof(*existing)); free(existing);
                return problem(req, 409, "urn:sms-gateway:idempotency-expired", "Conflict", "Idempotent message record is no longer retained");
            }
            err = send_message_record(req, 202, existing);
            gateway_security_wipe(existing, sizeof(*existing)); free(existing);
            return err;
        }
        idem_claimed = true;
    }

    uint32_t message_id = 0;
    err = modem_core_sms_send(to->valuestring, text->valuestring, delivery, &message_id);
    if (err != ESP_OK && idem_claimed) {
        (void)gateway_idempotency_release_pending(idem_key, fingerprint);
    }
    if (err == ESP_OK && idem_claimed) {
        for (unsigned attempt = 0; attempt < 3 && !idem_finalized; ++attempt) {
            idem_finalized = gateway_idempotency_finalize(idem_key, fingerprint, message_id) == ESP_OK;
        }
        if (!idem_finalized) {
            httpd_resp_set_hdr(req, "X-Idempotency-State", "pending");
            ESP_LOGE(TAG, "SMS id=%" PRIu32 " queued but idempotency finalization is pending", message_id);
        }
    }
    cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
    gateway_security_wipe(idem_key, sizeof(idem_key)); gateway_security_wipe(fingerprint, sizeof(fingerprint));
    if (err != ESP_OK) return problem(req, err == ESP_ERR_NO_MEM ? 507 : 503, "urn:sms-gateway:queue-failed", "Service Unavailable", "SMS could not be durably queued");

    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) return ESP_ERR_NO_MEM;
    err = modem_core_sms_get(message_id, message);
    if (err != ESP_OK) { free(message); return problem(req, 500, "urn:sms-gateway:store-read", "Internal Server Error", "Queued message could not be read back"); }
    char location[64];
    snprintf(location, sizeof(location), "/api/v1/messages/%" PRIu32, message_id);
    httpd_resp_set_hdr(req, "Location", location);
    err = send_message_record(req, 202, message);
    gateway_security_wipe(message, sizeof(*message)); free(message);
    return err;
}

static esp_err_t message_item_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, req->method == HTTP_POST)) return ESP_OK;
    uint32_t id = 0; bool retry = false;
    if (!parse_message_id(req->uri, &id, &retry)) return problem(req, 404, "urn:sms-gateway:not-found", "Not Found", "Unknown message resource");
    if (retry) {
        if (req->method != HTTP_POST) return problem(req, 405, "urn:sms-gateway:method", "Method Not Allowed", NULL);
        char *body = NULL;
        if (read_body(req, &body) != ESP_OK) return problem(req, 400, "urn:sms-gateway:invalid-body", "Bad Request", NULL);
        cJSON *json = cJSON_Parse(body);
        static const char *const allowed[] = {"acknowledge_duplicate_risk"};
        const bool valid_object = json != NULL && json_has_only_fields(json, allowed, sizeof(allowed) / sizeof(allowed[0]));
        const cJSON *ack = valid_object ? cJSON_GetObjectItemCaseSensitive(json, "acknowledge_duplicate_risk") : NULL;
        const bool accepted = cJSON_IsTrue(ack);
        cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
        if (!accepted) return problem(req, 400, "urn:sms-gateway:duplicate-risk", "Bad Request", "Explicit duplicate-risk acknowledgement is required");
        const esp_err_t err = modem_core_sms_retry_uncertain(id);
        if (err == ESP_ERR_NOT_FOUND) return problem(req, 404, "urn:sms-gateway:not-found", "Not Found", NULL);
        if (err != ESP_OK) return problem(req, 409, "urn:sms-gateway:not-uncertain", "Conflict", "Message is not retryable");
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_send(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    if (req->method == HTTP_DELETE) {
        const esp_err_t err = modem_core_sms_delete(id);
        if (err == ESP_ERR_NOT_FOUND) return problem(req, 404, "urn:sms-gateway:not-found", "Not Found", NULL);
        if (err != ESP_OK) return problem(req, 409, "urn:sms-gateway:not-deletable", "Conflict", "Message is currently in-flight");
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    if (req->method != HTTP_GET) return problem(req, 405, "urn:sms-gateway:method", "Method Not Allowed", NULL);
    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) return ESP_ERR_NO_MEM;
    const esp_err_t err = modem_core_sms_get(id, message);
    if (err != ESP_OK) { free(message); return problem(req, 404, "urn:sms-gateway:not-found", "Not Found", NULL); }
    const esp_err_t sent = send_message_record(req, 200, message);
    gateway_security_wipe(message, sizeof(*message)); free(message);
    return sent;
}

static esp_err_t sim_pin_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK) return problem(req, 400, "urn:sms-gateway:invalid-body", "Bad Request", NULL);
    cJSON *json = cJSON_Parse(body);
    static const char *const allowed[] = {"pin"};
    const bool valid_object = json != NULL && json_has_only_fields(json, allowed, sizeof(allowed) / sizeof(allowed[0]));
    const cJSON *pin = valid_object ? cJSON_GetObjectItemCaseSensitive(json, "pin") : NULL;
    esp_err_t err = cJSON_IsString(pin) ? modem_core_submit_sim_pin(pin->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json); gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
    if (err != ESP_OK) return problem(req, 409, "urn:sms-gateway:sim-pin", "Conflict", "SIM is not currently accepting that PIN request");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

static cJSON *mqtt_config_json(const gateway_mqtt_config_t *config)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;
    cJSON_AddBoolToObject(obj, "enabled", config->enabled);
    cJSON_AddStringToObject(obj, "broker_uri", config->broker_uri);
    cJSON_AddStringToObject(obj, "username", config->username);
    cJSON_AddBoolToObject(obj, "password_set", config->password[0] != '\0');
    cJSON_AddStringToObject(obj, "base_topic", config->base_topic);
    cJSON_AddBoolToObject(obj, "home_assistant_enabled", config->home_assistant_enabled);
    cJSON_AddStringToObject(obj, "discovery_prefix", config->discovery_prefix);
    if (config->default_recipient[0] != '\0') cJSON_AddStringToObject(obj, "default_recipient", config->default_recipient);
    else cJSON_AddNullToObject(obj, "default_recipient");
    cJSON_AddBoolToObject(obj, "ca_configured", config->ca_pem[0] != '\0');
    cJSON_AddBoolToObject(obj, "tls", gateway_mqtt_uri_is_tls(config->broker_uri));
    mqtt_service_diagnostics_t diag = {0};
    if (mqtt_service_get_diagnostics(&diag) == ESP_OK) {
        cJSON_AddBoolToObject(obj, "connected", diag.connected);
        cJSON_AddNumberToObject(obj, "last_mqtt_error", diag.last_mqtt_error);
    }
    return obj;
}

static bool json_copy_optional_string(cJSON *json, const char *name, char *dst, size_t dst_size, bool nullable)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    if (value == NULL) return true;
    if (nullable && cJSON_IsNull(value)) { dst[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || value->valuestring == NULL || strlen(value->valuestring) >= dst_size) return false;
    snprintf(dst, dst_size, "%s", value->valuestring);
    return true;
}

static bool json_copy_optional_bool(cJSON *json, const char *name, bool *dst)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    if (value == NULL) return true;
    if (!cJSON_IsBool(value)) return false;
    *dst = cJSON_IsTrue(value);
    return true;
}

static esp_err_t mqtt_config_get_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    gateway_mqtt_config_t config = {0};
    if (gateway_settings_get_mqtt(&config) != ESP_OK) return problem(req, 503, "urn:sms-gateway:mqtt-config", "Service Unavailable", "MQTT configuration is unavailable");
    cJSON *obj = mqtt_config_json(&config);
    gateway_security_wipe(&config, sizeof(config));
    if (obj == NULL) return ESP_ERR_NO_MEM;
    const esp_err_t err = send_json(req, 200, obj);
    cJSON_Delete(obj);
    return err;
}

static esp_err_t mqtt_config_patch_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK) return problem(req, 400, "urn:sms-gateway:invalid-body", "Bad Request", NULL);
    cJSON *json = cJSON_Parse(body);
    static const char *const allowed[] = {
        "enabled", "broker_uri", "username", "password", "base_topic",
        "home_assistant_enabled", "discovery_prefix", "default_recipient", "ca_pem"
    };
    gateway_mqtt_config_t config = {0};
    esp_err_t err = gateway_settings_get_mqtt(&config);
    if (json == NULL || !json_has_only_fields(json, allowed, sizeof(allowed) / sizeof(allowed[0])) || err != ESP_OK ||
        !json_copy_optional_bool(json, "enabled", &config.enabled) ||
        !json_copy_optional_string(json, "broker_uri", config.broker_uri, sizeof(config.broker_uri), false) ||
        !json_copy_optional_string(json, "username", config.username, sizeof(config.username), false) ||
        !json_copy_optional_string(json, "password", config.password, sizeof(config.password), true) ||
        !json_copy_optional_string(json, "base_topic", config.base_topic, sizeof(config.base_topic), false) ||
        !json_copy_optional_bool(json, "home_assistant_enabled", &config.home_assistant_enabled) ||
        !json_copy_optional_string(json, "discovery_prefix", config.discovery_prefix, sizeof(config.discovery_prefix), false) ||
        !json_copy_optional_string(json, "default_recipient", config.default_recipient, sizeof(config.default_recipient), true) ||
        !json_copy_optional_string(json, "ca_pem", config.ca_pem, sizeof(config.ca_pem), true) ||
        gateway_mqtt_config_validate(&config) != ESP_OK) {
        cJSON_Delete(json);
        gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
        gateway_security_wipe(&config, sizeof(config));
        return problem(req, 400, "urn:sms-gateway:invalid-mqtt-config", "Bad Request", "Invalid MQTT configuration");
    }
    err = gateway_settings_set_mqtt(&config);
    if (err == ESP_OK) err = mqtt_service_reconfigure();
    cJSON_Delete(json);
    gateway_security_wipe(body, (size_t)req->content_len + 1); free(body);
    if (err != ESP_OK) {
        gateway_security_wipe(&config, sizeof(config));
        return problem(req, 503, "urn:sms-gateway:mqtt-reconfigure", "Service Unavailable", "MQTT configuration was saved but the runtime could not apply it");
    }
    cJSON *out = mqtt_config_json(&config);
    gateway_security_wipe(&config, sizeof(config));
    if (out == NULL) return ESP_ERR_NO_MEM;
    err = send_json(req, 200, out);
    cJSON_Delete(out);
    return err;
}

static esp_err_t idempotency_clear_pending_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    char *body = NULL;
    esp_err_t err = read_body(req, &body);
    if (err != ESP_OK) return problem(req, 400, "urn:sms-gateway:invalid-body", "Bad Request", "Request body is missing or too large");
    cJSON *json = cJSON_Parse(body);
    const cJSON *ack = json ? cJSON_GetObjectItemCaseSensitive(json, "acknowledge_duplicate_risk") : NULL;
    static const char *const allowed[] = {"acknowledge_duplicate_risk"};
    const bool approved = json != NULL && json_has_only_fields(json, allowed, 1) && cJSON_IsTrue(ack);
    cJSON_Delete(json);
    gateway_security_wipe(body, (size_t)req->content_len + 1U);
    free(body);
    if (!approved) {
        return problem(req, 409, "urn:sms-gateway:duplicate-risk-not-acknowledged", "Conflict",
                       "Clearing pending idempotency reservations can permit duplicate SMS; explicit acknowledgement is required");
    }
    uint32_t cleared = 0;
    err = gateway_idempotency_clear_pending(&cleared);
    if (err != ESP_OK) return problem(req, 503, "urn:sms-gateway:idempotency-recovery", "Service Unavailable", "Pending reservations could not be cleared");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "cleared", cleared);
    cJSON_AddStringToObject(out, "warning", "Cleared reservations may permit duplicate sends if their original outcome was unknown");
    err = send_json(req, 200, out);
    cJSON_Delete(out);
    return err;
}

static esp_err_t modem_restart_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    const esp_err_t err = modem_core_restart_modem();
    if (err != ESP_OK) return problem(req, 503, "urn:sms-gateway:modem-restart", "Service Unavailable", "Modem restart could not be queued");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!authorized(req) || !request_allowed(req, false)) return ESP_OK;
    httpd_resp_set_status(req, "202 Accepted");
    (void)httpd_resp_send(req, NULL, 0);
    (void)xTaskCreate(reboot_task, "api_reboot", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t route = {.uri = uri, .method = method, .handler = handler, .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &route));
}

esp_err_t api_server_init(void)
{
    if (s_server != NULL) return ESP_ERR_INVALID_STATE;
    gateway_rate_limiter_init(&s_request_limiter, 60.0, 1.0, now_ms());
    gateway_rate_limiter_init(&s_sms_limiter, 5.0, 1.0 / 6.0, now_ms());
    gateway_rate_limiter_init(&s_ota_limiter, 2.0, 1.0 / 300.0, now_ms());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "http server");

    register_uri("/api/v1/health", HTTP_GET, health_handler);
    register_uri("/api/v1/status", HTTP_GET, status_handler);
    register_uri("/api/v1/messages", HTTP_GET, messages_get_handler);
    register_uri("/api/v1/messages", HTTP_POST, messages_post_handler);
    register_uri("/api/v1/messages/*", HTTP_GET, message_item_handler);
    register_uri("/api/v1/messages/*", HTTP_DELETE, message_item_handler);
    register_uri("/api/v1/messages/*", HTTP_POST, message_item_handler);
    register_uri("/api/v1/config/mqtt", HTTP_GET, mqtt_config_get_handler);
    register_uri("/api/v1/config/mqtt", HTTP_PATCH, mqtt_config_patch_handler);
    register_uri("/api/v1/modem/sim-pin", HTTP_POST, sim_pin_handler);
    register_uri("/api/v1/modem/restart", HTTP_POST, modem_restart_handler);
    register_uri("/api/v1/system/idempotency/clear-pending", HTTP_POST, idempotency_clear_pending_handler);
    register_uri("/api/v1/system/firmware", HTTP_GET, firmware_get_handler);
    register_uri("/api/v1/system/firmware", HTTP_POST, firmware_post_handler);
    register_uri("/api/v1/system/reboot", HTTP_POST, reboot_handler);
    ESP_LOGW(TAG, "REST API is HTTP-only; use a trusted LAN/reverse TLS proxy until device TLS credentials are implemented");
    return ESP_OK;
}
