#include "sms_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SMS_STORE_NAMESPACE "sms"
#define SMS_STORE_RECORD_MAGIC 0x534D5347UL /* SMSG */
#define SMS_STORE_RECORD_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t id;
    uint8_t direction;
    uint8_t status;
    uint8_t encoding;
    uint8_t delivery_report_requested;
    char sender[SMS_MAX_ADDRESS_LENGTH];
    char recipient[SMS_MAX_ADDRESS_LENGTH];
    char service_center_timestamp[40];
    char text[SMS_MESSAGE_TEXT_MAX];
    char raw_pdu[SMS_MAX_PDU_HEX];
    uint8_t concat_present;
    uint8_t concat_is_16bit;
    uint16_t concat_reference;
    uint8_t concat_total_parts;
    uint8_t concat_part_number;
    uint16_t outbound_concat_reference;
    uint8_t segment_count;
    uint16_t segment_sent_mask;
    uint16_t segment_delivered_mask;
    uint16_t segment_failed_mask;
    uint8_t modem_reference[SMS_MAX_SEGMENTS];
    uint8_t delivery_status[SMS_MAX_SEGMENTS];
    uint8_t inflight_segment;
    uint8_t send_attempts;
    int32_t last_modem_error;
} persisted_record_t;

static const char *TAG = "sms_store";
static nvs_handle_t s_handle;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;
/* All store operations are serialized by s_mutex, so these large buffers are shared safely. */
static persisted_record_t s_io_record;
static sms_message_t s_scan_record;
static sms_message_t s_sort_record;
static uint32_t s_pruned_records;
static uint32_t s_capacity_failures;
static uint32_t s_replay_watermark;
static bool s_replay_protection_enabled = true;

static bool terminated_field(const char *field, size_t capacity)
{
    return field != NULL && memchr(field, '\0', capacity) != NULL;
}

static void record_encode(const sms_message_t *message, persisted_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = SMS_STORE_RECORD_MAGIC;
    record->version = SMS_STORE_RECORD_VERSION;
    record->size = (uint16_t)sizeof(*record);
    record->id = message->id;
    record->direction = (uint8_t)message->direction;
    record->status = (uint8_t)message->status;
    record->encoding = (uint8_t)message->encoding;
    record->delivery_report_requested = message->delivery_report_requested ? 1U : 0U;
    memcpy(record->sender, message->sender, sizeof(record->sender));
    memcpy(record->recipient, message->recipient, sizeof(record->recipient));
    memcpy(record->service_center_timestamp, message->service_center_timestamp,
           sizeof(record->service_center_timestamp));
    memcpy(record->text, message->text, sizeof(record->text));
    memcpy(record->raw_pdu, message->raw_pdu, sizeof(record->raw_pdu));
    record->concat_present = message->concat.present ? 1U : 0U;
    record->concat_is_16bit = message->concat.is_16bit ? 1U : 0U;
    record->concat_reference = message->concat.reference;
    record->concat_total_parts = message->concat.total_parts;
    record->concat_part_number = message->concat.part_number;
    record->outbound_concat_reference = message->concat_reference;
    record->segment_count = message->segment_count;
    record->segment_sent_mask = message->segment_sent_mask;
    record->segment_delivered_mask = message->segment_delivered_mask;
    record->segment_failed_mask = message->segment_failed_mask;
    memcpy(record->modem_reference, message->modem_reference, sizeof(record->modem_reference));
    memcpy(record->delivery_status, message->delivery_status, sizeof(record->delivery_status));
    record->inflight_segment = message->inflight_segment;
    record->send_attempts = message->send_attempts;
    record->last_modem_error = message->last_modem_error;
}

static bool record_decode(const persisted_record_t *record, sms_message_t *message)
{
    if (record->magic != SMS_STORE_RECORD_MAGIC || record->version != SMS_STORE_RECORD_VERSION ||
        record->size != sizeof(*record) || record->id == 0 ||
        record->direction > SMS_DIRECTION_OUTBOUND || record->status > SMS_MESSAGE_UNCERTAIN ||
        record->encoding > SMS_ENCODING_UNKNOWN || record->segment_count > SMS_MAX_SEGMENTS ||
        !terminated_field(record->sender, sizeof(record->sender)) ||
        !terminated_field(record->recipient, sizeof(record->recipient)) ||
        !terminated_field(record->service_center_timestamp, sizeof(record->service_center_timestamp)) ||
        !terminated_field(record->text, sizeof(record->text)) ||
        !terminated_field(record->raw_pdu, sizeof(record->raw_pdu))) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->id = record->id;
    message->direction = (sms_direction_t)record->direction;
    message->status = (sms_message_status_t)record->status;
    message->encoding = (sms_encoding_t)record->encoding;
    message->delivery_report_requested = record->delivery_report_requested != 0;
    memcpy(message->sender, record->sender, sizeof(message->sender));
    memcpy(message->recipient, record->recipient, sizeof(message->recipient));
    memcpy(message->service_center_timestamp, record->service_center_timestamp,
           sizeof(message->service_center_timestamp));
    memcpy(message->text, record->text, sizeof(message->text));
    memcpy(message->raw_pdu, record->raw_pdu, sizeof(message->raw_pdu));
    message->concat.present = record->concat_present != 0;
    message->concat.is_16bit = record->concat_is_16bit != 0;
    message->concat.reference = record->concat_reference;
    message->concat.total_parts = record->concat_total_parts;
    message->concat.part_number = record->concat_part_number;
    message->concat_reference = record->outbound_concat_reference;
    message->segment_count = record->segment_count;
    message->segment_sent_mask = record->segment_sent_mask;
    message->segment_delivered_mask = record->segment_delivered_mask;
    message->segment_failed_mask = record->segment_failed_mask;
    memcpy(message->modem_reference, record->modem_reference, sizeof(message->modem_reference));
    memcpy(message->delivery_status, record->delivery_status, sizeof(message->delivery_status));
    message->inflight_segment = record->inflight_segment;
    message->send_attempts = record->send_attempts;
    message->last_modem_error = record->last_modem_error;
    return true;
}

static void slot_key(size_t slot, char key[8])
{
    (void)snprintf(key, 8, "s%03u", (unsigned)slot);
}

static esp_err_t load_slot(size_t slot, sms_message_t *out, bool *occupied)
{
    char key[8];
    slot_key(slot, key);
    size_t length = sizeof(s_io_record);
    esp_err_t err = nvs_get_blob(s_handle, key, &s_io_record, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (occupied != NULL) {
            *occupied = false;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(s_io_record) || !record_decode(&s_io_record, &s_scan_record)) {
        ESP_LOGE(TAG, "invalid record in slot %u", (unsigned)slot);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out != NULL) {
        *out = s_scan_record;
    }
    if (occupied != NULL) {
        *occupied = true;
    }
    return ESP_OK;
}

static esp_err_t write_slot(size_t slot, const sms_message_t *message)
{
    char key[8];
    slot_key(slot, key);
    record_encode(message, &s_io_record);
    esp_err_t err = nvs_set_blob(s_handle, key, &s_io_record, sizeof(s_io_record));
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}

static esp_err_t find_id_slot(uint32_t id, size_t *slot_out)
{
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        esp_err_t err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) {
            return err;
        }
        if (occupied && s_scan_record.id == id) {
            *slot_out = slot;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t find_empty_slot(size_t *slot_out)
{
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        esp_err_t err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) {
            return err;
        }
        if (!occupied) {
            *slot_out = slot;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static bool record_prunable(const sms_message_t *message)
{
    if (message == NULL) return false;
    switch (message->status) {
    case SMS_MESSAGE_RECEIVED:
        return !s_replay_protection_enabled || message->id <= s_replay_watermark;
    case SMS_MESSAGE_SENT:
        return !message->delivery_report_requested;
    case SMS_MESSAGE_DELIVERED:
    case SMS_MESSAGE_FAILED:
    case SMS_MESSAGE_UNSUPPORTED:
        return true;
    case SMS_MESSAGE_PARTIAL:
    case SMS_MESSAGE_QUEUED:
    case SMS_MESSAGE_SENDING:
    case SMS_MESSAGE_UNCERTAIN:
    default:
        return false;
    }
}

static esp_err_t find_prunable_slot(size_t *slot_out)
{
    bool found = false;
    uint32_t oldest_id = UINT32_MAX;
    size_t oldest_slot = 0;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        esp_err_t err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) return err;
        if (occupied && record_prunable(&s_scan_record) && s_scan_record.id < oldest_id) {
            oldest_id = s_scan_record.id;
            oldest_slot = slot;
            found = true;
        }
    }
    if (!found) return ESP_ERR_NO_MEM;
    *slot_out = oldest_slot;
    return ESP_OK;
}

static esp_err_t erase_slot(size_t slot)
{
    char key[8];
    slot_key(slot, key);
    esp_err_t err = nvs_erase_key(s_handle, key);
    if (err == ESP_OK) err = nvs_commit(s_handle);
    return err;
}

static esp_err_t allocate_id(uint32_t *id_out)
{
    uint32_t next = 1;
    esp_err_t err = nvs_get_u32(s_handle, "next_id", &next);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    if (next == 0) {
        next = 1;
    }
    *id_out = next;
    ++next;
    if (next == 0) {
        next = 1;
    }
    return nvs_set_u32(s_handle, "next_id", next);
}

esp_err_t sms_store_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_flash_init_partition(SMS_STORE_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize %s NVS partition: %s",
                 SMS_STORE_PARTITION, esp_err_to_name(err));
        return err;
    }
    err = nvs_open_from_partition(SMS_STORE_PARTITION, SMS_STORE_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        return err;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        nvs_close(s_handle);
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "durable SMS store ready (%d slots)", SMS_STORE_MAX_RECORDS);
    return ESP_OK;
}

esp_err_t sms_store_save(sms_message_t *message)
{
    if (!s_initialized || message == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!terminated_field(message->text, sizeof(message->text)) ||
        !terminated_field(message->sender, sizeof(message->sender)) ||
        !terminated_field(message->recipient, sizeof(message->recipient)) ||
        !terminated_field(message->service_center_timestamp, sizeof(message->service_center_timestamp)) ||
        !terminated_field(message->raw_pdu, sizeof(message->raw_pdu)) ||
        message->segment_count > SMS_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t slot;
    const bool is_new = message->id == 0;
    esp_err_t err;
    if (is_new) {
        err = find_empty_slot(&slot);
        if (err == ESP_ERR_NO_MEM) {
            err = find_prunable_slot(&slot);
            if (err == ESP_OK) {
                err = erase_slot(slot);
                if (err == ESP_OK) ++s_pruned_records;
            }
        }
        if (err == ESP_OK) {
            err = allocate_id(&message->id);
        } else if (err == ESP_ERR_NO_MEM) {
            ++s_capacity_failures;
        }
    } else {
        err = find_id_slot(message->id, &slot);
        if (err == ESP_ERR_NOT_FOUND) {
            err = find_empty_slot(&slot);
        }
    }
    if (err == ESP_OK) {
        err = write_slot(slot, message);
        if (err != ESP_OK && is_new) {
            message->id = 0;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_get(uint32_t id, sms_message_t *out)
{
    if (!s_initialized || id == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t slot;
    esp_err_t err = find_id_slot(id, &slot);
    if (err == ESP_OK) {
        err = load_slot(slot, out, NULL);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_delete(uint32_t id)
{
    if (!s_initialized || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t slot;
    esp_err_t err = find_id_slot(id, &slot);
    if (err == ESP_OK) {
        char key[8];
        slot_key(slot, key);
        err = nvs_erase_key(s_handle, key);
        if (err == ESP_OK) {
            err = nvs_commit(s_handle);
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_find_status(sms_message_status_t status, sms_message_t *out)
{
    if (!s_initialized || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    uint32_t best_id = UINT32_MAX;
    size_t best_slot = 0;
    esp_err_t err = ESP_OK;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) {
            break;
        }
        if (occupied && s_scan_record.status == status && s_scan_record.id < best_id) {
            best_id = s_scan_record.id;
            best_slot = slot;
            found = true;
        }
    }
    if (err == ESP_OK && found) {
        err = load_slot(best_slot, out, NULL);
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_find_partial(const char *sender,
                                 uint16_t reference,
                                 uint8_t total_parts,
                                 uint8_t part_number,
                                 sms_message_t *out)
{
    if (!s_initialized || sender == NULL || out == NULL || total_parts == 0 || part_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        esp_err_t slot_err = load_slot(slot, NULL, &occupied);
        if (slot_err != ESP_OK) {
            err = slot_err;
            break;
        }
        if (occupied && s_scan_record.direction == SMS_DIRECTION_INBOUND &&
            s_scan_record.status == SMS_MESSAGE_PARTIAL && s_scan_record.concat.present &&
            s_scan_record.concat.reference == reference && s_scan_record.concat.total_parts == total_parts &&
            s_scan_record.concat.part_number == part_number && strcmp(s_scan_record.sender, sender) == 0) {
            *out = s_scan_record;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_find_inbound_identity(const char *sender,
                                          const char *service_center_timestamp,
                                          sms_encoding_t encoding,
                                          const char *text,
                                          sms_message_t *out)
{
    if (!s_initialized || sender == NULL || service_center_timestamp == NULL || text == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        const esp_err_t slot_err = load_slot(slot, NULL, &occupied);
        if (slot_err != ESP_OK) {
            err = slot_err;
            break;
        }
        if (occupied && s_scan_record.direction == SMS_DIRECTION_INBOUND &&
            s_scan_record.status == SMS_MESSAGE_RECEIVED && s_scan_record.encoding == encoding &&
            strcmp(s_scan_record.sender, sender) == 0 &&
            strcmp(s_scan_record.service_center_timestamp, service_center_timestamp) == 0 &&
            strcmp(s_scan_record.text, text) == 0) {
            *out = s_scan_record;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_find_inbound_raw_pdu(const char *pdu_hex, sms_message_t *out)
{
    if (!s_initialized || pdu_hex == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        const esp_err_t slot_err = load_slot(slot, NULL, &occupied);
        if (slot_err != ESP_OK) {
            err = slot_err;
            break;
        }
        if (occupied && s_scan_record.direction == SMS_DIRECTION_INBOUND &&
            s_scan_record.status == SMS_MESSAGE_UNSUPPORTED &&
            strcmp(s_scan_record.raw_pdu, pdu_hex) == 0) {
            *out = s_scan_record;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sms_store_find_outbound_reference(const char *recipient,
                                            uint8_t modem_reference,
                                            sms_message_t *out,
                                            size_t *segment_index)
{
    if (!s_initialized || recipient == NULL || out == NULL || segment_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    uint32_t best_id = 0;
    size_t best_slot = 0;
    size_t best_index = 0;
    esp_err_t err = ESP_OK;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) {
            break;
        }
        if (!occupied || s_scan_record.direction != SMS_DIRECTION_OUTBOUND ||
            strcmp(s_scan_record.recipient, recipient) != 0) {
            continue;
        }
        for (size_t i = 0; i < s_scan_record.segment_count; ++i) {
            if ((s_scan_record.segment_sent_mask & (1U << i)) != 0U &&
                s_scan_record.modem_reference[i] == modem_reference && (!found || s_scan_record.id > best_id)) {
                best_id = s_scan_record.id;
                best_slot = slot;
                best_index = i;
                found = true;
            }
        }
    }
    if (err == ESP_OK && found) {
        err = load_slot(best_slot, out, NULL);
        if (err == ESP_OK) {
            *segment_index = best_index;
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

static void insertion_sort(sms_message_t *records, size_t count)
{
    /* Store mutex is held, so a shared workspace avoids a multi-kilobyte stack copy. */
    for (size_t i = 1; i < count; ++i) {
        s_sort_record = records[i];
        size_t j = i;
        while (j > 0 && records[j - 1U].id > s_sort_record.id) {
            records[j] = records[j - 1U];
            --j;
        }
        records[j] = s_sort_record;
    }
}

esp_err_t sms_store_list(uint32_t after_id,
                         sms_message_t *records,
                         size_t max_records,
                         size_t *record_count)
{
    if (!s_initialized || records == NULL || max_records == 0 || record_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = 0;
    esp_err_t err = ESP_OK;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) {
            break;
        }
        if (!occupied || s_scan_record.id <= after_id) {
            continue;
        }
        if (count < max_records) {
            records[count++] = s_scan_record;
        } else {
            size_t highest = 0;
            for (size_t i = 1; i < count; ++i) {
                if (records[i].id > records[highest].id) {
                    highest = i;
                }
            }
            if (s_scan_record.id < records[highest].id) {
                records[highest] = s_scan_record;
            }
        }
    }
    if (err == ESP_OK) {
        insertion_sort(records, count);
        *record_count = count;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void sms_store_set_replay_watermark(bool protection_enabled, uint32_t watermark)
{
    if (!s_initialized || s_mutex == NULL) {
        s_replay_protection_enabled = protection_enabled;
        s_replay_watermark = watermark;
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_replay_protection_enabled = protection_enabled;
    s_replay_watermark = watermark;
    xSemaphoreGive(s_mutex);
}

esp_err_t sms_store_get_diagnostics(sms_store_diagnostics_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    out->pruned_records = s_pruned_records;
    out->capacity_failures = s_capacity_failures;
    out->replay_watermark = s_replay_watermark;
    out->replay_protection_enabled = s_replay_protection_enabled;
    for (size_t slot = 0; slot < SMS_STORE_MAX_RECORDS; ++slot) {
        bool occupied = false;
        esp_err_t err = load_slot(slot, NULL, &occupied);
        if (err != ESP_OK) { xSemaphoreGive(s_mutex); return err; }
        if (occupied) ++out->used_records;
    }
    out->free_records = (uint16_t)(SMS_STORE_MAX_RECORDS - out->used_records);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

const char *sms_message_status_name(sms_message_status_t status)
{
    switch (status) {
    case SMS_MESSAGE_PARTIAL: return "partial";
    case SMS_MESSAGE_RECEIVED: return "received";
    case SMS_MESSAGE_QUEUED: return "queued";
    case SMS_MESSAGE_SENDING: return "sending";
    case SMS_MESSAGE_SENT: return "sent";
    case SMS_MESSAGE_DELIVERED: return "delivered";
    case SMS_MESSAGE_FAILED: return "failed";
    case SMS_MESSAGE_UNSUPPORTED: return "unsupported";
    case SMS_MESSAGE_UNCERTAIN: return "uncertain";
    default: return "unknown";
    }
}
