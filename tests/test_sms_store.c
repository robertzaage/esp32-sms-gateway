#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/semphr.h"

typedef struct {
    char key[8];
    unsigned char *data;
    size_t length;
    int used;
} fake_blob_t;

static fake_blob_t blobs[SMS_STORE_MAX_RECORDS];
static uint32_t next_id;
static int has_next_id;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) { (void)semaphore; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { (void)semaphore; return pdTRUE; }

esp_err_t nvs_flash_init_partition(const char *partition)
{
    assert(strcmp(partition, SMS_STORE_PARTITION) == 0);
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *out)
{
    assert(strcmp(partition, SMS_STORE_PARTITION) == 0);
    assert(strcmp(name, "sms") == 0);
    assert(mode == NVS_READWRITE);
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

static fake_blob_t *blob_for(const char *key, int create)
{
    for (size_t i = 0; i < SMS_STORE_MAX_RECORDS; ++i) {
        if (blobs[i].used && strcmp(blobs[i].key, key) == 0) return &blobs[i];
    }
    if (!create) return NULL;
    for (size_t i = 0; i < SMS_STORE_MAX_RECORDS; ++i) {
        if (!blobs[i].used) {
            blobs[i].used = 1;
            strncpy(blobs[i].key, key, sizeof(blobs[i].key) - 1U);
            return &blobs[i];
        }
    }
    return NULL;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    (void)handle;
    fake_blob_t *blob = blob_for(key, 0);
    if (blob == NULL) return ESP_ERR_NVS_NOT_FOUND;
    if (length == NULL) return ESP_ERR_INVALID_ARG;
    if (out == NULL) { *length = blob->length; return ESP_OK; }
    if (*length < blob->length) return ESP_ERR_INVALID_SIZE;
    memcpy(out, blob->data, blob->length);
    *length = blob->length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    (void)handle;
    fake_blob_t *blob = blob_for(key, 1);
    if (blob == NULL) return ESP_ERR_NO_MEM;
    unsigned char *copy = malloc(length);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    memcpy(copy, data, length);
    free(blob->data);
    blob->data = copy;
    blob->length = length;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    (void)handle;
    assert(strcmp(key, "next_id") == 0);
    if (!has_next_id) return ESP_ERR_NVS_NOT_FOUND;
    *out = next_id;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    assert(strcmp(key, "next_id") == 0);
    next_id = value;
    has_next_id = 1;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    fake_blob_t *blob = blob_for(key, 0);
    if (blob == NULL) return ESP_ERR_NVS_NOT_FOUND;
    free(blob->data);
    memset(blob, 0, sizeof(*blob));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

static sms_message_t make_message(sms_direction_t direction, sms_message_status_t status, const char *text)
{
    sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.direction = direction;
    message.status = status;
    message.encoding = SMS_ENCODING_GSM7;
    message.segment_count = 1;
    memset(message.modem_reference, 0xFF, sizeof(message.modem_reference));
    memset(message.delivery_status, 0xFF, sizeof(message.delivery_status));
    strncpy(message.text, text, sizeof(message.text) - 1U);
    return message;
}

int main(void)
{
    assert(sms_store_init() == ESP_OK);

    sms_message_t first = make_message(SMS_DIRECTION_OUTBOUND, SMS_MESSAGE_QUEUED, "first");
    strcpy(first.recipient, "+491111");
    assert(sms_store_save(&first) == ESP_OK && first.id == 1);

    sms_message_t second = make_message(SMS_DIRECTION_OUTBOUND, SMS_MESSAGE_SENT, "second");
    strcpy(second.recipient, "+492222");
    second.segment_sent_mask = 1;
    second.modem_reference[0] = 42;
    assert(sms_store_save(&second) == ESP_OK && second.id == 2);

    sms_message_t got;
    assert(sms_store_get(first.id, &got) == ESP_OK);
    assert(strcmp(got.text, "first") == 0);
    first.status = SMS_MESSAGE_SENDING;
    assert(sms_store_save(&first) == ESP_OK);
    assert(sms_store_find_status(SMS_MESSAGE_SENDING, &got) == ESP_OK && got.id == first.id);

    sms_message_t partial = make_message(SMS_DIRECTION_INBOUND, SMS_MESSAGE_PARTIAL, "part two");
    strcpy(partial.sender, "+493333");
    partial.concat.present = true;
    partial.concat.reference = 0x1234;
    partial.concat.total_parts = 3;
    partial.concat.part_number = 2;
    partial.segment_count = 3;
    assert(sms_store_save(&partial) == ESP_OK);
    assert(sms_store_find_partial("+493333", 0x1234, 3, 2, &got) == ESP_OK);
    assert(got.id == partial.id && strcmp(got.text, "part two") == 0);

    sms_message_t inbound = make_message(SMS_DIRECTION_INBOUND, SMS_MESSAGE_RECEIVED, "dedup me");
    strcpy(inbound.sender, "+494444");
    strcpy(inbound.service_center_timestamp, "2026-08-23T15:00:00+02:00");
    assert(sms_store_save(&inbound) == ESP_OK);
    assert(sms_store_find_inbound_identity("+494444", "2026-08-23T15:00:00+02:00",
                                           SMS_ENCODING_GSM7, "dedup me", &got) == ESP_OK);
    assert(got.id == inbound.id);

    sms_message_t unsupported = make_message(SMS_DIRECTION_INBOUND, SMS_MESSAGE_UNSUPPORTED, "");
    unsupported.encoding = SMS_ENCODING_8BIT;
    strcpy(unsupported.raw_pdu, "001122AABB");
    assert(sms_store_save(&unsupported) == ESP_OK);
    assert(sms_store_find_inbound_raw_pdu("001122AABB", &got) == ESP_OK);
    assert(got.id == unsupported.id && got.status == SMS_MESSAGE_UNSUPPORTED);

    sms_message_t newer = make_message(SMS_DIRECTION_OUTBOUND, SMS_MESSAGE_SENT, "newer");
    strcpy(newer.recipient, "+492222");
    newer.segment_count = 2;
    newer.segment_sent_mask = 2;
    newer.modem_reference[1] = 42;
    assert(sms_store_save(&newer) == ESP_OK);
    size_t segment = 0;
    assert(sms_store_find_outbound_reference("+492222", 42, &got, &segment) == ESP_OK);
    assert(got.id == newer.id && segment == 1);

    sms_message_t listed[2];
    size_t count = 0;
    assert(sms_store_list(0, listed, 2, &count) == ESP_OK && count == 2);
    assert(listed[0].id == 1 && listed[1].id == 2);
    assert(sms_store_list(2, listed, 2, &count) == ESP_OK && count == 2);
    assert(listed[0].id == partial.id && listed[1].id == inbound.id);

    assert(sms_store_delete(second.id) == ESP_OK);
    assert(sms_store_get(second.id, &got) == ESP_ERR_NOT_FOUND);

    /* Protect unacknowledged inbound events from pruning, but allow terminal
     * records at/below the replay watermark to be evicted when full. */
    sms_store_set_replay_watermark(true, inbound.id);
    size_t existing = 5; /* first + partial + inbound + unsupported + newer; second was deleted */
    for (size_t i = existing; i < SMS_STORE_MAX_RECORDS; ++i) {
        sms_message_t message = make_message(SMS_DIRECTION_INBOUND, SMS_MESSAGE_RECEIVED, "capacity");
        assert(sms_store_save(&message) == ESP_OK);
    }
    sms_message_t overflow = make_message(SMS_DIRECTION_INBOUND, SMS_MESSAGE_RECEIVED, "overflow");
    assert(sms_store_save(&overflow) == ESP_OK); /* oldest safe terminal record is pruned */
    assert(overflow.id != 0);
    sms_store_diagnostics_t diag;
    assert(sms_store_get_diagnostics(&diag) == ESP_OK);
    assert(diag.pruned_records >= 1 && diag.used_records == SMS_STORE_MAX_RECORDS);
    assert(diag.replay_protection_enabled && diag.replay_watermark == inbound.id);

    for (size_t i = 0; i < SMS_STORE_MAX_RECORDS; ++i) {
        free(blobs[i].data);
        blobs[i].data = NULL;
    }
    puts("sms store tests passed");
    return 0;
}
