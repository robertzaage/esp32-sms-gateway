#include "api_idempotency.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define IDEM_NAMESPACE "api_idem"
#define IDEM_MAGIC 0x4944454dU
#define IDEM_VERSION 2U
#define IDEM_LOCK_TIMEOUT_MS 2000

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t message_id; /* 0 means a durable reservation is pending finalization. */
    uint8_t key_hash[GATEWAY_SHA256_LEN];
    uint8_t request_hash[GATEWAY_SHA256_LEN];
} idem_record_t;

static nvs_handle_t s_nvs;
static bool s_initialized;
static SemaphoreHandle_t s_mutex;

static bool equal32(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < GATEWAY_SHA256_LEN; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void slot_key(unsigned slot, char out[8])
{
    (void)snprintf(out, 8, "r%02u", slot);
}

static esp_err_t read_slot(unsigned slot, idem_record_t *record)
{
    char key[8];
    slot_key(slot, key);
    size_t size = sizeof(*record);
    esp_err_t err = nvs_get_blob(s_nvs, key, record, &size);
    if (err != ESP_OK) return err;
    if (size != sizeof(*record) || record->magic != IDEM_MAGIC || record->version != IDEM_VERSION) {
        gateway_security_wipe(record, sizeof(*record));
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t hash_key(const char *key, uint8_t out[GATEWAY_SHA256_LEN])
{
    if (key == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    return gateway_security_sha256(key, strlen(key), out);
}

static esp_err_t write_slot(unsigned slot, const idem_record_t *record)
{
    char key[8];
    slot_key(slot, key);
    esp_err_t err = nvs_set_blob(s_nvs, key, record, sizeof(*record));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

static esp_err_t lock_store(void)
{
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(IDEM_LOCK_TIMEOUT_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_store(void)
{
    xSemaphoreGive(s_mutex);
}

esp_err_t gateway_idempotency_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open(IDEM_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) s_initialized = true;
    return err;
}

esp_err_t gateway_idempotency_sms_fingerprint(const char *recipient,
                                              const char *text,
                                              bool delivery_report,
                                              uint8_t out[GATEWAY_SHA256_LEN])
{
    if (recipient == NULL || text == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    const size_t a = strlen(recipient);
    const size_t b = strlen(text);
    if (a > SIZE_MAX - b - 3) return ESP_ERR_INVALID_SIZE;
    const size_t len = a + b + 3;
    uint8_t *buffer = malloc(len);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    memcpy(buffer, recipient, a);
    buffer[a] = 0;
    memcpy(buffer + a + 1, text, b);
    buffer[a + 1 + b] = 0;
    buffer[len - 1] = delivery_report ? 1U : 0U;
    const esp_err_t err = gateway_security_sha256(buffer, len, out);
    gateway_security_wipe(buffer, len);
    free(buffer);
    return err;
}

esp_err_t gateway_idempotency_lookup(const char *key,
                                     const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                     gateway_idempotency_result_t *result,
                                     uint32_t *message_id)
{
    if (key == NULL || request_hash == NULL || result == NULL || message_id == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    uint8_t key_hash[GATEWAY_SHA256_LEN] = {0};
    err = hash_key(key, key_hash);
    if (err != ESP_OK) goto out;
    *result = GATEWAY_IDEMPOTENCY_MISS;
    *message_id = 0;
    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        err = read_slot(i, &record);
        if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_VERSION) { err = ESP_OK; continue; }
        if (err != ESP_OK) break;
        if (equal32(record.key_hash, key_hash)) {
            *message_id = record.message_id;
            *result = equal32(record.request_hash, request_hash) ? GATEWAY_IDEMPOTENCY_REPLAY : GATEWAY_IDEMPOTENCY_CONFLICT;
            gateway_security_wipe(&record, sizeof(record));
            break;
        }
        gateway_security_wipe(&record, sizeof(record));
    }
out:
    gateway_security_wipe(key_hash, sizeof(key_hash));
    unlock_store();
    return err;
}

esp_err_t gateway_idempotency_claim(const char *key,
                                    const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                    gateway_idempotency_result_t *result,
                                    uint32_t *message_id)
{
    if (key == NULL || request_hash == NULL || result == NULL || message_id == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    uint8_t key_hash[GATEWAY_SHA256_LEN] = {0};
    err = hash_key(key, key_hash);
    if (err != ESP_OK) goto out;

    int empty_slot = -1;
    int oldest_finalized_slot = -1;
    uint32_t oldest_sequence = UINT32_MAX;
    uint32_t max_sequence = 0;
    *result = GATEWAY_IDEMPOTENCY_MISS;
    *message_id = 0;

    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        err = read_slot(i, &record);
        if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_VERSION) {
            if (empty_slot < 0) empty_slot = (int)i;
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) goto out;
        if (record.sequence > max_sequence) max_sequence = record.sequence;
        if (record.message_id != 0 && record.sequence < oldest_sequence) {
            oldest_sequence = record.sequence;
            oldest_finalized_slot = (int)i;
        }
        if (equal32(record.key_hash, key_hash)) {
            *message_id = record.message_id;
            *result = equal32(record.request_hash, request_hash) ? GATEWAY_IDEMPOTENCY_REPLAY : GATEWAY_IDEMPOTENCY_CONFLICT;
            gateway_security_wipe(&record, sizeof(record));
            err = ESP_OK;
            goto out;
        }
        gateway_security_wipe(&record, sizeof(record));
    }

    if (empty_slot < 0 && oldest_finalized_slot < 0) { err = ESP_ERR_NO_MEM; goto out; }
    const unsigned chosen = empty_slot >= 0 ? (unsigned)empty_slot : (unsigned)oldest_finalized_slot;
    idem_record_t reservation = {
        .magic = IDEM_MAGIC,
        .version = IDEM_VERSION,
        .sequence = max_sequence == UINT32_MAX ? 1U : max_sequence + 1U,
        .message_id = 0,
    };
    memcpy(reservation.key_hash, key_hash, sizeof(reservation.key_hash));
    memcpy(reservation.request_hash, request_hash, sizeof(reservation.request_hash));
    err = write_slot(chosen, &reservation);
    gateway_security_wipe(&reservation, sizeof(reservation));
out:
    gateway_security_wipe(key_hash, sizeof(key_hash));
    unlock_store();
    return err;
}

esp_err_t gateway_idempotency_finalize(const char *key,
                                       const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                       uint32_t message_id)
{
    if (key == NULL || request_hash == NULL || message_id == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    uint8_t key_hash[GATEWAY_SHA256_LEN] = {0};
    err = hash_key(key, key_hash);
    if (err != ESP_OK) goto out;
    err = ESP_ERR_NOT_FOUND;
    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        esp_err_t read_err = read_slot(i, &record);
        if (read_err == ESP_ERR_NVS_NOT_FOUND || read_err == ESP_ERR_INVALID_VERSION) continue;
        if (read_err != ESP_OK) { err = read_err; break; }
        if (equal32(record.key_hash, key_hash)) {
            if (!equal32(record.request_hash, request_hash)) err = ESP_ERR_INVALID_STATE;
            else { record.message_id = message_id; err = write_slot(i, &record); }
            gateway_security_wipe(&record, sizeof(record));
            break;
        }
        gateway_security_wipe(&record, sizeof(record));
    }
out:
    gateway_security_wipe(key_hash, sizeof(key_hash));
    unlock_store();
    return err;
}

esp_err_t gateway_idempotency_release_pending(const char *key,
                                              const uint8_t request_hash[GATEWAY_SHA256_LEN])
{
    if (key == NULL || request_hash == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    uint8_t key_hash[GATEWAY_SHA256_LEN] = {0};
    err = hash_key(key, key_hash);
    if (err != ESP_OK) goto out;
    err = ESP_ERR_NOT_FOUND;
    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        esp_err_t read_err = read_slot(i, &record);
        if (read_err == ESP_ERR_NVS_NOT_FOUND || read_err == ESP_ERR_INVALID_VERSION) continue;
        if (read_err != ESP_OK) { err = read_err; break; }
        if (equal32(record.key_hash, key_hash)) {
            if (!equal32(record.request_hash, request_hash) || record.message_id != 0) {
                err = ESP_ERR_INVALID_STATE;
            } else {
                char slot[8]; slot_key(i, slot);
                err = nvs_erase_key(s_nvs, slot);
                if (err == ESP_OK) err = nvs_commit(s_nvs);
            }
            gateway_security_wipe(&record, sizeof(record));
            break;
        }
        gateway_security_wipe(&record, sizeof(record));
    }
out:
    gateway_security_wipe(key_hash, sizeof(key_hash));
    unlock_store();
    return err;
}

esp_err_t gateway_idempotency_remember(const char *key,
                                       const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                       uint32_t message_id)
{
    gateway_idempotency_result_t result = GATEWAY_IDEMPOTENCY_MISS;
    uint32_t existing = 0;
    esp_err_t err = gateway_idempotency_claim(key, request_hash, &result, &existing);
    if (err != ESP_OK) return err;
    if (result == GATEWAY_IDEMPOTENCY_CONFLICT) return ESP_ERR_INVALID_STATE;
    if (result == GATEWAY_IDEMPOTENCY_REPLAY && existing != 0 && existing != message_id) return ESP_ERR_INVALID_STATE;
    return gateway_idempotency_finalize(key, request_hash, message_id);
}


esp_err_t gateway_idempotency_get_diagnostics(gateway_idempotency_diagnostics_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        esp_err_t read_err = read_slot(i, &record);
        if (read_err == ESP_ERR_NVS_NOT_FOUND || read_err == ESP_ERR_INVALID_VERSION) {
            ++out->free_records;
            continue;
        }
        if (read_err != ESP_OK) { err = read_err; break; }
        if (record.message_id == 0) ++out->pending_records;
        else ++out->finalized_records;
        gateway_security_wipe(&record, sizeof(record));
    }
    unlock_store();
    return err;
}

esp_err_t gateway_idempotency_clear_pending(uint32_t *cleared)
{
    if (cleared == NULL) return ESP_ERR_INVALID_ARG;
    *cleared = 0;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < GATEWAY_IDEMPOTENCY_MAX_RECORDS; ++i) {
        idem_record_t record = {0};
        esp_err_t read_err = read_slot(i, &record);
        if (read_err == ESP_ERR_NVS_NOT_FOUND || read_err == ESP_ERR_INVALID_VERSION) continue;
        if (read_err != ESP_OK) { err = read_err; break; }
        if (record.message_id == 0) {
            char slot[8]; slot_key(i, slot);
            err = nvs_erase_key(s_nvs, slot);
            if (err != ESP_OK) { gateway_security_wipe(&record, sizeof(record)); break; }
            ++(*cleared);
        }
        gateway_security_wipe(&record, sizeof(record));
    }
    if (err == ESP_OK && *cleared > 0) err = nvs_commit(s_nvs);
    unlock_store();
    return err;
}
