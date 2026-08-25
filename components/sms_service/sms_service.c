#include "sms_service.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SMS_SERVICE_QUEUE_DEPTH 32
#define SMS_SERVICE_TASK_STACK 12288
#define SMS_SERVICE_TASK_PRIORITY 9
#define SMS_SERVICE_WAKE_MS 2000
#define SMS_SERVICE_AT_TIMEOUT_MS 10000
#define SMS_SERVICE_CMGS_TIMEOUT_MS 60000
#define SMS_SERVICE_URC_MAX AT_ENGINE_MAX_URC_LENGTH

typedef enum {
    SERVICE_EVENT_MODEM_READY = 0,
    SERVICE_EVENT_MODEM_LOST,
    SERVICE_EVENT_URC,
    SERVICE_EVENT_WAKE,
} service_event_id_t;

typedef struct {
    service_event_id_t id;
    char urc[SMS_SERVICE_URC_MAX];
} service_event_t;

typedef enum {
    DIRECT_URC_NONE = 0,
    DIRECT_URC_DELIVER,
    DIRECT_URC_STATUS_REPORT,
} direct_urc_pending_t;

typedef struct {
    bool initialized;
    sms_service_transport_t transport;
    sms_service_event_callback_t event_cb;
    void *event_user_ctx;
    QueueHandle_t queue;
    TaskHandle_t task;
    direct_urc_pending_t direct_pending;
    sms_service_diagnostics_t diagnostics;
} service_state_t;

static const char *TAG = "sms_service";
static service_state_t s_service;
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;

static size_t bounded_strlen(const char *text, size_t limit)
{
    if (text == NULL) {
        return 0;
    }
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

static void diag_set_error(esp_err_t err, int cms_error)
{
    portENTER_CRITICAL(&s_diag_lock);
    s_service.diagnostics.last_error = err;
    if (cms_error >= 0) {
        s_service.diagnostics.last_cms_error = cms_error;
    }
    portEXIT_CRITICAL(&s_diag_lock);
}

static void diag_increment(uint32_t *counter)
{
    portENTER_CRITICAL(&s_diag_lock);
    ++*counter;
    portEXIT_CRITICAL(&s_diag_lock);
}

static void emit_event(sms_service_event_t event, const sms_message_t *message)
{
    if (s_service.event_cb != NULL && message != NULL) {
        s_service.event_cb(event, message, s_service.event_user_ctx);
    }
}

static bool modem_ready(void)
{
    return s_service.transport.is_ready != NULL && s_service.transport.is_ready(s_service.transport.ctx);
}

static esp_err_t execute(const at_request_t *request, at_response_t *response)
{
    if (!modem_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_service.transport.execute(s_service.transport.ctx, request, response);
}

static bool execute_simple(const char *command,
                           const char *const *prefixes,
                           size_t prefix_count,
                           uint32_t timeout_ms,
                           at_response_t *response)
{
    at_request_t request = {
        .command = command,
        .expected_prefixes = prefixes,
        .expected_prefix_count = prefix_count,
        .timeout_ms = timeout_ms,
        .max_attempts = 1,
        .retry_policy = AT_RETRY_NONE,
    };
    at_response_t local;
    if (response == NULL) {
        response = &local;
    }
    const esp_err_t err = execute(&request, response);
    if (err != ESP_OK) {
        diag_set_error(err, -1);
        return false;
    }
    if (response->result != AT_RESULT_OK) {
        diag_set_error(ESP_FAIL, response->result == AT_RESULT_CMS_ERROR ? response->error_code : -1);
        return false;
    }
    return true;
}

static bool parse_index_urc(const char *line, const char *prefix, int *index)
{
    if (line == NULL || prefix == NULL || index == NULL || strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *comma = strrchr(line, ',');
    if (comma == NULL) {
        return false;
    }
    char *end = NULL;
    long value = strtol(comma + 1, &end, 10);
    while (end != NULL && isspace((unsigned char)*end)) {
        ++end;
    }
    if (end == comma + 1 || end == NULL || *end != '\0' || value < 0 || value > 65535) {
        return false;
    }
    *index = (int)value;
    return true;
}

static bool parse_cmgl_index(const char *line, int *index)
{
    if (line == NULL || index == NULL || strncmp(line, "+CMGL:", 6) != 0) {
        return false;
    }
    const char *p = line + 6;
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p || value < 0 || value > 65535) {
        return false;
    }
    *index = (int)value;
    return true;
}

static bool parse_cmgs_reference(const at_response_t *response, uint8_t *reference)
{
    if (response == NULL || reference == NULL) {
        return false;
    }
    for (size_t i = 0; i < response->line_count; ++i) {
        const char *line = at_response_line(response, i);
        if (line == NULL || strncmp(line, "+CMGS:", 6) != 0) {
            continue;
        }
        const char *p = line + 6;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end != p && value >= 0 && value <= 255) {
            *reference = (uint8_t)value;
            return true;
        }
    }
    return false;
}

static bool is_hex_pdu_line(const char *line)
{
    if (line == NULL) {
        return false;
    }
    const size_t len = strlen(line);
    if (len < 4 || (len & 1U) != 0U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)line[i])) {
            return false;
        }
    }
    return sms_pdu_type_detect(line) != SMS_PDU_TYPE_UNKNOWN;
}

static esp_err_t delete_modem_index(int index)
{
    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CMGD=%d,0", index);
    return execute_simple(command, NULL, 0, SMS_SERVICE_AT_TIMEOUT_MS, NULL) ? ESP_OK : ESP_FAIL;
}

static esp_err_t assemble_if_complete(const sms_deliver_t *deliver, sms_message_t *assembled, bool *created)
{
    if (deliver == NULL || assembled == NULL || created == NULL || !deliver->concat.present) {
        return ESP_ERR_INVALID_ARG;
    }
    *created = false;
    memset(assembled, 0, sizeof(*assembled));
    assembled->direction = SMS_DIRECTION_INBOUND;
    assembled->status = SMS_MESSAGE_RECEIVED;
    assembled->encoding = deliver->encoding;
    strncpy(assembled->sender, deliver->sender, sizeof(assembled->sender) - 1U);
    strncpy(assembled->service_center_timestamp, deliver->service_center_timestamp,
            sizeof(assembled->service_center_timestamp) - 1U);
    assembled->segment_count = deliver->concat.total_parts;
    assembled->concat_reference = deliver->concat.reference;

    sms_message_t *partial = calloc(1, sizeof(*partial));
    if (partial == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t output_len = 0;
    uint32_t partial_ids[SMS_MAX_SEGMENTS] = {0};
    esp_err_t result = ESP_OK;
    for (uint8_t part = 1; part <= deliver->concat.total_parts; ++part) {
        memset(partial, 0, sizeof(*partial));
        result = sms_store_find_partial(deliver->sender,
                                        deliver->concat.reference,
                                        deliver->concat.total_parts,
                                        part,
                                        partial);
        if (result != ESP_OK) {
            goto cleanup;
        }
        const size_t length = bounded_strlen(partial->text, sizeof(partial->text));
        if (length >= sizeof(partial->text) || output_len + length >= sizeof(assembled->text)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        memcpy(assembled->text + output_len, partial->text, length);
        output_len += length;
        assembled->text[output_len] = '\0';
        partial_ids[part - 1U] = partial->id;
        if (part == 1) {
            strncpy(assembled->service_center_timestamp, partial->service_center_timestamp,
                    sizeof(assembled->service_center_timestamp) - 1U);
        }
    }

    sms_message_t *existing = calloc(1, sizeof(*existing));
    if (existing == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    const esp_err_t duplicate_err = sms_store_find_inbound_identity(assembled->sender,
                                                                     assembled->service_center_timestamp,
                                                                     assembled->encoding,
                                                                     assembled->text,
                                                                     existing);
    if (duplicate_err == ESP_OK) {
        *assembled = *existing;
        diag_increment(&s_service.diagnostics.duplicate_messages);
    } else if (duplicate_err == ESP_ERR_NOT_FOUND) {
        result = sms_store_save(assembled);
        if (result == ESP_OK) {
            *created = true;
        }
    } else {
        result = duplicate_err;
    }
    free(existing);
    if (result != ESP_OK) {
        goto cleanup;
    }
    for (uint8_t part = 1; part <= deliver->concat.total_parts; ++part) {
        const esp_err_t delete_err = sms_store_delete(partial_ids[part - 1U]);
        if (delete_err != ESP_OK) {
            ESP_LOGW(TAG, "assembled message=%" PRIu32 " but could not delete partial=%" PRIu32,
                     assembled->id, partial_ids[part - 1U]);
        }
    }

cleanup:
    free(partial);
    return result;
}

static esp_err_t persist_deliver(const sms_deliver_t *deliver, sms_message_t *event_message)
{
    if (deliver == NULL || event_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sms_message_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return ESP_ERR_NO_MEM;
    }
    record->direction = SMS_DIRECTION_INBOUND;
    record->status = deliver->concat.present ? SMS_MESSAGE_PARTIAL : SMS_MESSAGE_RECEIVED;
    record->encoding = deliver->encoding;
    record->concat = deliver->concat;
    record->concat_reference = deliver->concat.reference;
    record->segment_count = deliver->concat.present ? deliver->concat.total_parts : 1;
    strncpy(record->sender, deliver->sender, sizeof(record->sender) - 1U);
    strncpy(record->service_center_timestamp, deliver->service_center_timestamp,
            sizeof(record->service_center_timestamp) - 1U);
    strncpy(record->text, deliver->text, sizeof(record->text) - 1U);

    esp_err_t result = ESP_OK;
    if (deliver->concat.present) {
        sms_message_t *existing = calloc(1, sizeof(*existing));
        sms_message_t *assembled = calloc(1, sizeof(*assembled));
        if (existing == NULL || assembled == NULL) {
            free(existing);
            free(assembled);
            free(record);
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t existing_err = sms_store_find_partial(deliver->sender,
                                                               deliver->concat.reference,
                                                               deliver->concat.total_parts,
                                                               deliver->concat.part_number,
                                                               existing);
        if (existing_err == ESP_OK) {
            if (strcmp(existing->text, deliver->text) == 0) {
                diag_increment(&s_service.diagnostics.duplicate_parts);
                *event_message = *existing;
            } else {
                existing->encoding = deliver->encoding;
                existing->concat = deliver->concat;
                strncpy(existing->text, deliver->text, sizeof(existing->text) - 1U);
                strncpy(existing->service_center_timestamp, deliver->service_center_timestamp,
                        sizeof(existing->service_center_timestamp) - 1U);
                result = sms_store_save(existing);
                if (result == ESP_OK) {
                    *event_message = *existing;
                }
            }
        } else if (existing_err == ESP_ERR_NOT_FOUND) {
            result = sms_store_save(record);
            if (result == ESP_OK) {
                *event_message = *record;
                diag_increment(&s_service.diagnostics.inbound_parts);
            }
        } else {
            result = existing_err;
        }

        if (result == ESP_OK) {
            bool created = false;
            const esp_err_t assembly_err = assemble_if_complete(deliver, assembled, &created);
            if (assembly_err == ESP_OK) {
                *event_message = *assembled;
                if (created) {
                    diag_increment(&s_service.diagnostics.inbound_messages);
                    emit_event(SMS_SERVICE_EVENT_RECEIVED, assembled);
                }
            } else if (assembly_err == ESP_ERR_NOT_FOUND) {
                emit_event(SMS_SERVICE_EVENT_PARTIAL_RECEIVED, event_message);
            } else {
                result = assembly_err;
            }
        }
        free(existing);
        free(assembled);
        free(record);
        return result;
    }

    sms_message_t *existing = calloc(1, sizeof(*existing));
    if (existing == NULL) {
        free(record);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t duplicate_err = sms_store_find_inbound_identity(record->sender,
                                                                     record->service_center_timestamp,
                                                                     record->encoding,
                                                                     record->text,
                                                                     existing);
    if (duplicate_err == ESP_OK) {
        *event_message = *existing;
        diag_increment(&s_service.diagnostics.duplicate_messages);
        result = ESP_OK;
    } else if (duplicate_err == ESP_ERR_NOT_FOUND) {
        result = sms_store_save(record);
        if (result == ESP_OK) {
            *event_message = *record;
            diag_increment(&s_service.diagnostics.inbound_messages);
            emit_event(SMS_SERVICE_EVENT_RECEIVED, record);
        }
    } else {
        result = duplicate_err;
    }
    free(existing);
    free(record);
    return result;
}

static esp_err_t process_status_report_pdu(const char *pdu)
{
    sms_status_report_t report;
    const sms_pdu_decode_result_t decode = sms_status_report_decode(pdu, &report);
    if (decode != SMS_PDU_DECODE_OK) {
        diag_increment(&s_service.diagnostics.decode_failures);
        return ESP_ERR_INVALID_RESPONSE;
    }
    diag_increment(&s_service.diagnostics.delivery_reports);

    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t segment = 0;
    esp_err_t err = sms_store_find_outbound_reference(report.recipient,
                                                       report.message_reference,
                                                       message,
                                                       &segment);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmatched delivery report mr=%u", (unsigned)report.message_reference);
        free(message);
        return err;
    }
    message->delivery_status[segment] = report.status;
    if (report.delivered) {
        message->segment_delivered_mask |= (uint16_t)(1U << segment);
    } else if (report.status >= 0x40U) {
        message->segment_failed_mask |= (uint16_t)(1U << segment);
    }

    const uint16_t all = message->segment_count >= 16U ? 0xFFFFU
                                                        : (uint16_t)((1U << message->segment_count) - 1U);
    if ((message->segment_delivered_mask & all) == all) {
        message->status = SMS_MESSAGE_DELIVERED;
    } else if ((message->segment_failed_mask & all) != 0U) {
        message->status = SMS_MESSAGE_FAILED;
    }
    err = sms_store_save(message);
    if (err == ESP_OK) {
        if (message->status == SMS_MESSAGE_DELIVERED) {
            emit_event(SMS_SERVICE_EVENT_DELIVERED, message);
        } else if (message->status == SMS_MESSAGE_FAILED) {
            emit_event(SMS_SERVICE_EVENT_FAILED, message);
        }
    }
    free(message);
    return err;
}

static esp_err_t persist_unsupported_deliver(const char *pdu,
                                                const sms_deliver_t *deliver,
                                                sms_message_t *stored)
{
    if (pdu == NULL || deliver == NULL || stored == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sms_message_t *existing = calloc(1, sizeof(*existing));
    if (existing == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sms_store_find_inbound_raw_pdu(pdu, existing);
    if (err == ESP_OK) {
        *stored = *existing;
        diag_increment(&s_service.diagnostics.duplicate_messages);
        free(existing);
        return ESP_OK;
    }
    free(existing);
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    memset(stored, 0, sizeof(*stored));
    stored->direction = SMS_DIRECTION_INBOUND;
    stored->status = SMS_MESSAGE_UNSUPPORTED;
    stored->encoding = deliver->encoding;
    stored->segment_count = 1;
    strncpy(stored->sender, deliver->sender, sizeof(stored->sender) - 1U);
    strncpy(stored->service_center_timestamp, deliver->service_center_timestamp,
            sizeof(stored->service_center_timestamp) - 1U);
    strncpy(stored->raw_pdu, pdu, sizeof(stored->raw_pdu) - 1U);
    err = sms_store_save(stored);
    if (err == ESP_OK) {
        diag_increment(&s_service.diagnostics.unsupported_pdus);
        emit_event(SMS_SERVICE_EVENT_UNSUPPORTED_RECEIVED, stored);
    }
    return err;
}

static esp_err_t process_deliver_pdu(const char *pdu, int modem_index)
{
    sms_deliver_t *deliver = calloc(1, sizeof(*deliver));
    sms_message_t *stored = calloc(1, sizeof(*stored));
    if (deliver == NULL || stored == NULL) {
        free(deliver);
        free(stored);
        return ESP_ERR_NO_MEM;
    }
    const sms_pdu_decode_result_t decode = sms_deliver_decode(pdu, deliver);
    if (decode == SMS_PDU_DECODE_UNSUPPORTED && deliver->encoding == SMS_ENCODING_8BIT) {
        diag_increment(&s_service.diagnostics.inbound_pdus);
        esp_err_t err = persist_unsupported_deliver(pdu, deliver, stored);
        if (err == ESP_OK && modem_index >= 0) {
            const esp_err_t delete_err = delete_modem_index(modem_index);
            if (delete_err != ESP_OK) {
                ESP_LOGW(TAG, "unsupported PDU persisted but modem index %d could not be deleted", modem_index);
            }
        }
        free(deliver);
        free(stored);
        return err;
    }
    if (decode != SMS_PDU_DECODE_OK) {
        ESP_LOGW(TAG, "unable to decode SMS-DELIVER result=%d", decode);
        diag_increment(&s_service.diagnostics.decode_failures);
        free(deliver);
        free(stored);
        return ESP_ERR_INVALID_RESPONSE;
    }
    diag_increment(&s_service.diagnostics.inbound_pdus);

    esp_err_t err = persist_deliver(deliver, stored);
    if (err != ESP_OK) {
        diag_increment(&s_service.diagnostics.store_failures);
        diag_set_error(err, -1);
        free(deliver);
        free(stored);
        return err;
    }

    /* Delete from SIM/ME only after durable persistence/assembly state exists. */
    if (modem_index >= 0) {
        const esp_err_t delete_err = delete_modem_index(modem_index);
        if (delete_err != ESP_OK) {
            ESP_LOGW(TAG, "message persisted but modem index %d could not be deleted", modem_index);
        }
    }
    free(deliver);
    free(stored);
    return ESP_OK;
}

static esp_err_t process_any_pdu(const char *pdu, int modem_index)
{
    switch (sms_pdu_type_detect(pdu)) {
    case SMS_PDU_TYPE_DELIVER:
        return process_deliver_pdu(pdu, modem_index);
    case SMS_PDU_TYPE_STATUS_REPORT: {
        esp_err_t err = process_status_report_pdu(pdu);
        if (err == ESP_OK && modem_index >= 0) {
            (void)delete_modem_index(modem_index);
        }
        return err;
    }
    case SMS_PDU_TYPE_UNKNOWN:
    default:
        diag_increment(&s_service.diagnostics.decode_failures);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t read_modem_index(int index)
{
    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CMGR=%d", index);
    static const char *const prefixes[] = {"+CMGR:"};
    at_response_t response;
    if (!execute_simple(command, prefixes, 1, SMS_SERVICE_AT_TIMEOUT_MS, &response)) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < response.line_count; ++i) {
        const char *line = at_response_line(&response, i);
        if (is_hex_pdu_line(line)) {
            return process_any_pdu(line, index);
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static void scan_stored_messages(void)
{
    static const char *const prefixes[] = {"+CMGL:"};
    /*
     * at_response_t is deliberately bounded. Drain processed messages and repeat so
     * a modem inbox larger than AT_ENGINE_MAX_RESPONSE_LINES is still emptied.
     */
    for (size_t batch = 0; batch < SMS_STORE_MAX_RECORDS; ++batch) {
        at_response_t response;
        if (!execute_simple("AT+CMGL=4", prefixes, 1, 30000, &response)) {
            ESP_LOGW(TAG, "stored-message scan failed at batch %u", (unsigned)batch);
            return;
        }
        int pending_index = -1;
        size_t processed = 0;
        for (size_t i = 0; i < response.line_count; ++i) {
            const char *line = at_response_line(&response, i);
            int index;
            if (parse_cmgl_index(line, &index)) {
                pending_index = index;
                continue;
            }
            if (pending_index >= 0 && is_hex_pdu_line(line)) {
                if (process_any_pdu(line, pending_index) == ESP_OK) {
                    ++processed;
                }
                pending_index = -1;
            }
        }
        if (processed == 0) {
            return;
        }
    }
    ESP_LOGW(TAG, "stored-message scan reached safety batch limit");
}

static bool configure_pdu_mode(void)
{
    if (!execute_simple("AT+CMGF=0", NULL, 0, SMS_SERVICE_AT_TIMEOUT_MS, NULL)) {
        return false;
    }
    if (!execute_simple("AT+CNMI=2,1,0,1,0", NULL, 0, SMS_SERVICE_AT_TIMEOUT_MS, NULL)) {
        return false;
    }
    portENTER_CRITICAL(&s_diag_lock);
    s_service.diagnostics.pdu_mode_configured = true;
    portEXIT_CRITICAL(&s_diag_lock);
    scan_stored_messages();
    return true;
}

static void mark_interrupted_sends_uncertain(void)
{
    sms_message_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        diag_set_error(ESP_ERR_NO_MEM, -1);
        return;
    }
    uint32_t after = 0;
    for (;;) {
        size_t count = 0;
        if (sms_store_list(after, record, 1, &count) != ESP_OK || count == 0) {
            break;
        }
        if (record->direction == SMS_DIRECTION_OUTBOUND &&
            record->status == SMS_MESSAGE_SENDING) {
            record->status = SMS_MESSAGE_UNCERTAIN;
            (void)sms_store_save(record);
            emit_event(SMS_SERVICE_EVENT_UNCERTAIN, record);
        }
        after = record->id;
    }
    free(record);
}

static esp_err_t send_segment(sms_message_t *message,
                              const sms_submit_segment_t *segment,
                              size_t index)
{
    if (message == NULL || segment == NULL || index >= message->segment_count) {
        return ESP_ERR_INVALID_ARG;
    }

    message->status = SMS_MESSAGE_SENDING;
    message->inflight_segment = (uint8_t)(index + 1U);
    ++message->send_attempts;
    esp_err_t err = sms_store_save(message);
    if (err != ESP_OK) {
        return err;
    }

    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CMGS=%u", (unsigned)segment->tpdu_length_octets);
    const size_t pdu_len = strlen(segment->pdu_hex);
    uint8_t *payload = malloc(pdu_len + 1U);
    if (payload == NULL) {
        /* No modem bytes have been sent yet, so this segment remains safely retryable. */
        message->status = SMS_MESSAGE_QUEUED;
        message->inflight_segment = 0;
        (void)sms_store_save(message);
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload, segment->pdu_hex, pdu_len);
    payload[pdu_len] = 0x1A;

    static const char *const prefixes[] = {"+CMGS:"};
    const at_request_t request = {
        .command = command,
        .expected_prefixes = prefixes,
        .expected_prefix_count = 1,
        .timeout_ms = SMS_SERVICE_CMGS_TIMEOUT_MS,
        .max_attempts = 1, /* Never automatically duplicate a prompt transaction. */
        .retry_policy = AT_RETRY_NONE,
        .wait_for_prompt = true,
        .prompt_payload = payload,
        .prompt_payload_len = pdu_len + 1U,
    };
    at_response_t response;
    err = execute(&request, &response);
    free(payload);

    uint8_t modem_reference = 0;
    if (err != ESP_OK || response.result != AT_RESULT_OK || !parse_cmgs_reference(&response, &modem_reference)) {
        message->status = SMS_MESSAGE_UNCERTAIN;
        message->last_modem_error = (err == ESP_OK) ? response.error_code : -1;
        (void)sms_store_save(message);
        diag_increment(&s_service.diagnostics.outbound_uncertain);
        diag_set_error(err == ESP_OK ? ESP_FAIL : err,
                       (err == ESP_OK && response.result == AT_RESULT_CMS_ERROR) ? response.error_code : -1);
        emit_event(SMS_SERVICE_EVENT_UNCERTAIN, message);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    message->modem_reference[index] = modem_reference;
    message->segment_sent_mask |= (uint16_t)(1U << index);
    message->inflight_segment = 0;
    message->last_modem_error = 0;
    err = sms_store_save(message);
    if (err == ESP_OK) {
        diag_increment(&s_service.diagnostics.outbound_segments_sent);
    }
    return err;
}

static void process_one_outbound(void)
{
    bool pdu_ready;
    portENTER_CRITICAL(&s_diag_lock);
    pdu_ready = s_service.diagnostics.pdu_mode_configured;
    portEXIT_CRITICAL(&s_diag_lock);
    if (!modem_ready() || !pdu_ready) {
        return;
    }
    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) {
        diag_set_error(ESP_ERR_NO_MEM, -1);
        return;
    }
    if (sms_store_find_status(SMS_MESSAGE_QUEUED, message) != ESP_OK) {
        free(message);
        return;
    }

    sms_submit_segment_t *segments = calloc(SMS_MAX_SEGMENTS, sizeof(*segments));
    if (segments == NULL) {
        free(message);
        diag_set_error(ESP_ERR_NO_MEM, -1);
        return;
    }
    size_t segment_count = 0;
    if (!sms_submit_encode(message->recipient, message->text, message->delivery_report_requested,
                           message->concat_reference, segments, SMS_MAX_SEGMENTS, &segment_count) ||
        segment_count == 0 || segment_count != message->segment_count) {
        message->status = SMS_MESSAGE_FAILED;
        message->last_modem_error = -1;
        (void)sms_store_save(message);
        emit_event(SMS_SERVICE_EVENT_FAILED, message);
        free(segments);
        free(message);
        return;
    }

    for (size_t i = 0; i < segment_count; ++i) {
        if ((message->segment_sent_mask & (1U << i)) != 0U) {
            continue;
        }
        if (send_segment(message, &segments[i], i) != ESP_OK) {
            free(segments);
            free(message);
            return;
        }
    }
    free(segments);

    const uint16_t all = segment_count >= 16U ? 0xFFFFU
                                               : (uint16_t)((1U << segment_count) - 1U);
    if ((message->segment_sent_mask & all) == all) {
        message->status = SMS_MESSAGE_SENT;
        message->inflight_segment = 0;
        if (sms_store_save(message) == ESP_OK) {
            emit_event(SMS_SERVICE_EVENT_SENT, message);
        }
    }
    free(message);
}

static void handle_urc_line(const char *line)
{
    if (line == NULL) {
        return;
    }
    if (s_service.direct_pending != DIRECT_URC_NONE) {
        const direct_urc_pending_t pending = s_service.direct_pending;
        s_service.direct_pending = DIRECT_URC_NONE;
        if (is_hex_pdu_line(line)) {
            if (pending == DIRECT_URC_DELIVER) {
                (void)process_deliver_pdu(line, -1);
            } else {
                (void)process_status_report_pdu(line);
            }
        } else {
            diag_increment(&s_service.diagnostics.decode_failures);
        }
        return;
    }
    if (strncmp(line, "+CMT:", 5) == 0) {
        s_service.direct_pending = DIRECT_URC_DELIVER;
        return;
    }
    if (strncmp(line, "+CDS:", 5) == 0) {
        s_service.direct_pending = DIRECT_URC_STATUS_REPORT;
        return;
    }
    int index;
    if (parse_index_urc(line, "+CMTI:", &index) || parse_index_urc(line, "+CDSI:", &index)) {
        (void)read_modem_index(index);
    }
}

static void service_task(void *arg)
{
    (void)arg;
    mark_interrupted_sends_uncertain();

    for (;;) {
        service_event_t event;
        if (xQueueReceive(s_service.queue, &event, pdMS_TO_TICKS(SMS_SERVICE_WAKE_MS)) == pdTRUE) {
            switch (event.id) {
            case SERVICE_EVENT_MODEM_READY:
                portENTER_CRITICAL(&s_diag_lock);
                s_service.diagnostics.modem_ready = true;
                s_service.diagnostics.pdu_mode_configured = false;
                portEXIT_CRITICAL(&s_diag_lock);
                if (!configure_pdu_mode()) {
                    ESP_LOGW(TAG, "SMS PDU-mode initialization failed");
                }
                break;
            case SERVICE_EVENT_MODEM_LOST:
                s_service.direct_pending = DIRECT_URC_NONE;
                portENTER_CRITICAL(&s_diag_lock);
                s_service.diagnostics.modem_ready = false;
                s_service.diagnostics.pdu_mode_configured = false;
                portEXIT_CRITICAL(&s_diag_lock);
                break;
            case SERVICE_EVENT_URC:
                handle_urc_line(event.urc);
                break;
            case SERVICE_EVENT_WAKE:
            default:
                break;
            }
        }
        process_one_outbound();
    }
}

esp_err_t sms_service_init(const sms_service_transport_t *transport,
                           sms_service_event_callback_t event_cb,
                           void *user_ctx)
{
    if (s_service.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (transport == NULL || transport->execute == NULL || transport->is_ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sms_store_init();
    if (err != ESP_OK) {
        return err;
    }
    memset(&s_service, 0, sizeof(s_service));
    s_service.transport = *transport;
    s_service.event_cb = event_cb;
    s_service.event_user_ctx = user_ctx;
    s_service.diagnostics.last_cms_error = -1;
    s_service.queue = xQueueCreate(SMS_SERVICE_QUEUE_DEPTH, sizeof(service_event_t));
    if (s_service.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(service_task, "sms_service", SMS_SERVICE_TASK_STACK, NULL,
                    SMS_SERVICE_TASK_PRIORITY, &s_service.task) != pdPASS) {
        vQueueDelete(s_service.queue);
        s_service.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_service.initialized = true;
    s_service.diagnostics.initialized = true;
    return ESP_OK;
}

static void queue_simple_event(service_event_id_t id)
{
    if (!s_service.initialized || s_service.queue == NULL) {
        return;
    }
    const service_event_t event = {.id = id};
    if (xQueueSend(s_service.queue, &event, 0) != pdTRUE) {
        diag_increment(&s_service.diagnostics.urcs_dropped);
    }
}

void sms_service_notify_modem_ready(void)
{
    queue_simple_event(SERVICE_EVENT_MODEM_READY);
}

void sms_service_notify_modem_lost(void)
{
    queue_simple_event(SERVICE_EVENT_MODEM_LOST);
}

void sms_service_handle_urc(const char *line, void *user_ctx)
{
    (void)user_ctx;
    if (!s_service.initialized || line == NULL || s_service.queue == NULL) {
        return;
    }
    service_event_t event = {.id = SERVICE_EVENT_URC};
    const size_t len = bounded_strlen(line, sizeof(event.urc));
    if (len >= sizeof(event.urc)) {
        diag_increment(&s_service.diagnostics.urcs_dropped);
        return;
    }
    memcpy(event.urc, line, len + 1U);
    if (xQueueSend(s_service.queue, &event, 0) != pdTRUE) {
        diag_increment(&s_service.diagnostics.urcs_dropped);
    }
}

esp_err_t sms_service_send(const char *recipient,
                           const char *utf8_text,
                           bool request_delivery_report,
                           uint32_t *message_id)
{
    if (!s_service.initialized || recipient == NULL || utf8_text == NULL || message_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bounded_strlen(utf8_text, SMS_MESSAGE_TEXT_MAX) >= SMS_MESSAGE_TEXT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    sms_submit_segment_t *segments = calloc(SMS_MAX_SEGMENTS, sizeof(*segments));
    if (segments == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t concat_reference = (uint16_t)esp_random();
    if (concat_reference == 0) {
        concat_reference = 1;
    }
    size_t segment_count = 0;
    const bool valid = sms_submit_encode(recipient, utf8_text, request_delivery_report,
                                         concat_reference, segments, SMS_MAX_SEGMENTS, &segment_count);
    const sms_encoding_t encoding = valid && segment_count > 0 ? segments[0].encoding : SMS_ENCODING_UNKNOWN;
    free(segments);
    if (!valid || segment_count == 0 || segment_count > SMS_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) {
        return ESP_ERR_NO_MEM;
    }
    message->direction = SMS_DIRECTION_OUTBOUND;
    message->status = SMS_MESSAGE_QUEUED;
    message->encoding = encoding;
    message->delivery_report_requested = request_delivery_report;
    message->concat_reference = concat_reference;
    message->segment_count = (uint8_t)segment_count;
    memset(message->modem_reference, 0xFF, sizeof(message->modem_reference));
    memset(message->delivery_status, 0xFF, sizeof(message->delivery_status));
    strncpy(message->recipient, recipient, sizeof(message->recipient) - 1U);
    strncpy(message->text, utf8_text, sizeof(message->text) - 1U);

    esp_err_t err = sms_store_save(message);
    if (err == ESP_OK) {
        *message_id = message->id;
        diag_increment(&s_service.diagnostics.outbound_queued);
        queue_simple_event(SERVICE_EVENT_WAKE);
    }
    free(message);
    return err;
}

esp_err_t sms_service_get(uint32_t id, sms_message_t *out)
{
    return sms_store_get(id, out);
}

esp_err_t sms_service_list(uint32_t after_id,
                           sms_message_t *records,
                           size_t max_records,
                           size_t *record_count)
{
    return sms_store_list(after_id, records, max_records, record_count);
}

esp_err_t sms_service_delete(uint32_t id)
{
    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sms_store_get(id, message);
    if (err == ESP_OK && message->direction == SMS_DIRECTION_OUTBOUND &&
        (message->status == SMS_MESSAGE_QUEUED || message->status == SMS_MESSAGE_SENDING)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (err == ESP_OK) {
        err = sms_store_delete(id);
    }
    free(message);
    return err;
}

esp_err_t sms_service_retry_uncertain(uint32_t id)
{
    sms_message_t *message = calloc(1, sizeof(*message));
    if (message == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sms_store_get(id, message);
    if (err == ESP_OK &&
        (message->direction != SMS_DIRECTION_OUTBOUND || message->status != SMS_MESSAGE_UNCERTAIN)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (err == ESP_OK) {
        /* Explicit retry accepts the risk that the ambiguous in-flight segment was sent. */
        message->status = SMS_MESSAGE_QUEUED;
        message->inflight_segment = 0;
        err = sms_store_save(message);
        if (err == ESP_OK) {
            queue_simple_event(SERVICE_EVENT_WAKE);
        }
    }
    free(message);
    return err;
}

esp_err_t sms_service_get_diagnostics(sms_service_diagnostics_t *out)
{
    if (!s_service.initialized || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_diag_lock);
    *out = s_service.diagnostics;
    portEXIT_CRITICAL(&s_diag_lock);
    sms_store_diagnostics_t store = {0};
    if (sms_store_get_diagnostics(&store) == ESP_OK) {
        out->store_pruned_records = store.pruned_records;
        out->store_capacity_failures = store.capacity_failures;
        out->store_used_records = store.used_records;
        out->store_free_records = store.free_records;
    }
    return ESP_OK;
}

void sms_service_set_event_replay_watermark(bool protection_enabled, uint32_t watermark)
{
    sms_store_set_replay_watermark(protection_enabled, watermark);
}
