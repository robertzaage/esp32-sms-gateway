#include "ota_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_version_policy.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define OTA_PREFIX_REQUIRED (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define OTA_EXPECTED_PROJECT "esp32_sms_gateway"

static const char *TAG = "ota";
static SemaphoreHandle_t s_lock;
static gateway_ota_diagnostics_t s_diag;
static bool s_initialized;

struct gateway_ota_session {
    const esp_partition_t *partition;
    esp_ota_handle_t ota_handle;
    bool ota_started;
    bool hash_active;
    psa_hash_operation_t hash_op;
    size_t expected_size;
    size_t received;
    uint8_t expected_hash[32];
    uint8_t prefix[OTA_PREFIX_REQUIRED];
    size_t prefix_len;
    bool allow_reinstall;
    bool allow_downgrade;
    esp_err_t last_error;
    esp_app_desc_t candidate;
};

static void copy_text(char *dst, size_t size, const char *src)
{
    if (size == 0) return;
    if (src == NULL) src = "";
    snprintf(dst, size, "%s", src);
}

static void hex_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode_32(const char *text, uint8_t out[32])
{
    if (text == NULL || strlen(text) != GATEWAY_OTA_SHA256_HEX_LEN) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hex_value(text[i * 2]);
        const int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool constant_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void refresh_partition_diagnostics_locked(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_app_desc_t *running_desc = esp_app_get_description();
    copy_text(s_diag.running_version, sizeof(s_diag.running_version), running_desc ? running_desc->version : "");
    copy_text(s_diag.running_partition, sizeof(s_diag.running_partition), running ? running->label : "");
    copy_text(s_diag.boot_partition, sizeof(s_diag.boot_partition), boot ? boot->label : "");
    esp_app_desc_t desc = {0};
    if (boot != NULL && esp_ota_get_partition_description(boot, &desc) == ESP_OK) {
        copy_text(s_diag.boot_version, sizeof(s_diag.boot_version), desc.version);
    } else {
        s_diag.boot_version[0] = '\0';
    }
    s_diag.pending_verify = false;
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        s_diag.pending_verify = state == ESP_OTA_IMG_PENDING_VERIFY;
    }
}

static void session_release(gateway_ota_session_t *session, bool abort_ota)
{
    if (session == NULL) return;
    if (abort_ota && session->ota_started) {
        (void)esp_ota_abort(session->ota_handle);
        session->ota_started = false;
    }
    if (session->hash_active) {
        (void)psa_hash_abort(&session->hash_op);
        session->hash_active = false;
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_diag.update_in_progress = false;
        xSemaphoreGive(s_lock);
    }
    memset(session, 0, sizeof(*session));
    free(session);
}

static esp_err_t decision_error(ota_version_decision_t decision)
{
    switch (decision) {
    case OTA_VERSION_REJECT_PROJECT: return GATEWAY_OTA_ERR_PROJECT_MISMATCH;
    case OTA_VERSION_REJECT_FORMAT: return GATEWAY_OTA_ERR_VERSION_FORMAT;
    case OTA_VERSION_REJECT_REINSTALL: return GATEWAY_OTA_ERR_REINSTALL_REJECTED;
    case OTA_VERSION_REJECT_DOWNGRADE: return GATEWAY_OTA_ERR_DOWNGRADE_REJECTED;
    case OTA_VERSION_REJECT_SECURE_VERSION: return GATEWAY_OTA_ERR_SECURE_VERSION;
    case OTA_VERSION_ACCEPT:
    default: return ESP_OK;
    }
}

static esp_err_t validate_prefix_and_start(gateway_ota_session_t *session)
{
    esp_app_desc_t candidate = {0};
    const size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    memcpy(&candidate, session->prefix + desc_offset, sizeof(candidate));
    if (candidate.magic_word != ESP_APP_DESC_MAGIC_WORD) return ESP_ERR_OTA_VALIDATE_FAILED;
    candidate.version[sizeof(candidate.version) - 1] = '\0';
    candidate.project_name[sizeof(candidate.project_name) - 1] = '\0';

    const esp_app_desc_t *running = esp_app_get_description();
    if (running == NULL) return ESP_FAIL;
    const ota_version_decision_t decision = ota_version_policy_decide(
        OTA_EXPECTED_PROJECT, running->version, running->secure_version,
        candidate.project_name, candidate.version, candidate.secure_version,
        session->allow_reinstall, session->allow_downgrade);
    const esp_err_t policy_err = decision_error(decision);
    if (policy_err != ESP_OK) return policy_err;

    esp_err_t err = esp_ota_begin(session->partition, session->expected_size, &session->ota_handle);
    if (err != ESP_OK) return err;
    session->ota_started = true;
    err = esp_ota_write(session->ota_handle, session->prefix, session->prefix_len);
    if (err != ESP_OK) return err;
    session->candidate = candidate;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        copy_text(s_diag.last_candidate_version, sizeof(s_diag.last_candidate_version), candidate.version);
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

static void ota_confirm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_GATEWAY_OTA_CONFIRM_DELAY_SECONDS * 1000));
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = running != NULL ? esp_ota_get_state_partition(running, &state) : ESP_FAIL;
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) ESP_LOGI(TAG, "OTA image confirmed after stability window");
        else ESP_LOGE(TAG, "failed to confirm OTA image: %s", esp_err_to_name(err));
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_diag.confirmation_scheduled = false;
        s_diag.last_error = err;
        refresh_partition_diagnostics_locked();
        xSemaphoreGive(s_lock);
    }
    vTaskDelete(NULL);
}

esp_err_t gateway_ota_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    if (psa_crypto_init() != PSA_SUCCESS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        memset(&s_diag, 0, sizeof(s_diag));
        refresh_partition_diagnostics_locked();
        xSemaphoreGive(s_lock);
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t gateway_ota_mark_services_ready(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    bool schedule = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        refresh_partition_diagnostics_locked();
        if (s_diag.pending_verify && !s_diag.confirmation_scheduled) {
            s_diag.confirmation_scheduled = true;
            schedule = true;
        }
        xSemaphoreGive(s_lock);
    }
    if (!schedule) return ESP_OK;
    if (xTaskCreate(ota_confirm_task, "ota_confirm", 4096, NULL, 4, NULL) != pdPASS) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_diag.confirmation_scheduled = false;
            s_diag.last_error = ESP_ERR_NO_MEM;
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gateway_ota_get_diagnostics(gateway_ota_diagnostics_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    refresh_partition_diagnostics_locked();
    *out = s_diag;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t gateway_ota_begin(const gateway_ota_request_t *request, gateway_ota_session_t **out_session)
{
    if (!s_initialized || request == NULL || out_session == NULL || request->sha256_hex == NULL) return ESP_ERR_INVALID_ARG;
    *out_session = NULL;
    uint8_t parsed_hash[32] = {0};
    if (!hex_decode_32(request->sha256_hex, parsed_hash)) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        return GATEWAY_OTA_ERR_PENDING_VERIFY;
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || request->image_size < OTA_PREFIX_REQUIRED || request->image_size > partition->size) {
        return GATEWAY_OTA_ERR_IMAGE_SIZE;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (s_diag.update_in_progress) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_diag.update_in_progress = true;
    s_diag.upload_attempts++;
    xSemaphoreGive(s_lock);

    gateway_ota_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_diag.update_in_progress = false;
            s_diag.upload_failures++;
            s_diag.last_error = ESP_ERR_NO_MEM;
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    session->partition = partition;
    session->expected_size = request->image_size;
    session->allow_reinstall = request->allow_reinstall;
    session->allow_downgrade = request->allow_downgrade;
    memcpy(session->expected_hash, parsed_hash, sizeof(parsed_hash));
    memset(parsed_hash, 0, sizeof(parsed_hash));
    const psa_hash_operation_t initial_hash = PSA_HASH_OPERATION_INIT;
    session->hash_op = initial_hash;
    if (psa_hash_setup(&session->hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_diag.upload_failures++;
            s_diag.last_error = ESP_FAIL;
            xSemaphoreGive(s_lock);
        }
        session_release(session, false);
        return ESP_FAIL;
    }
    session->hash_active = true;
    *out_session = session;
    return ESP_OK;
}

esp_err_t gateway_ota_write(gateway_ota_session_t *session, const void *data, size_t len)
{
    if (session == NULL || (data == NULL && len != 0) || len == 0) return ESP_ERR_INVALID_ARG;
    if (session->received > session->expected_size || len > session->expected_size - session->received) {
        session->last_error = GATEWAY_OTA_ERR_IMAGE_SIZE;
        return session->last_error;
    }
    if (psa_hash_update(&session->hash_op, data, len) != PSA_SUCCESS) {
        session->last_error = ESP_FAIL;
        return session->last_error;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t consumed = 0;
    if (!session->ota_started) {
        const size_t need = sizeof(session->prefix) - session->prefix_len;
        const size_t copy = len < need ? len : need;
        memcpy(session->prefix + session->prefix_len, bytes, copy);
        session->prefix_len += copy;
        consumed += copy;
        if (session->prefix_len == sizeof(session->prefix)) {
            const esp_err_t err = validate_prefix_and_start(session);
            if (err != ESP_OK) { session->last_error = err; return err; }
        }
    }
    if (session->ota_started && consumed < len) {
        const esp_err_t err = esp_ota_write(session->ota_handle, bytes + consumed, len - consumed);
        if (err != ESP_OK) { session->last_error = err; return err; }
    }
    session->received += len;
    return ESP_OK;
}

esp_err_t gateway_ota_finish(gateway_ota_session_t *session, gateway_ota_result_t *result)
{
    if (session == NULL || result == NULL) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    esp_err_t err = ESP_OK;
    uint8_t digest[32] = {0};
    size_t digest_len = 0;

    if (session->received != session->expected_size || !session->ota_started) {
        err = GATEWAY_OTA_ERR_IMAGE_SIZE;
        goto done;
    }
    if (psa_hash_finish(&session->hash_op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != sizeof(digest)) {
        err = ESP_FAIL;
        goto done;
    }
    session->hash_active = false;
    if (!constant_equal(digest, session->expected_hash, sizeof(digest))) {
        err = GATEWAY_OTA_ERR_HASH_MISMATCH;
        goto done;
    }

    err = esp_ota_end(session->ota_handle);
    session->ota_started = false;
    if (err != ESP_OK) goto done;

    esp_app_desc_t written = {0};
    err = esp_ota_get_partition_description(session->partition, &written);
    if (err != ESP_OK) goto done;
    if (written.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(written.project_name, session->candidate.project_name, sizeof(written.project_name)) != 0 ||
        strncmp(written.version, session->candidate.version, sizeof(written.version)) != 0) {
        err = ESP_ERR_OTA_VALIDATE_FAILED;
        goto done;
    }
    err = esp_ota_set_boot_partition(session->partition);
    if (err != ESP_OK) goto done;

    copy_text(result->version, sizeof(result->version), written.version);
    copy_text(result->project_name, sizeof(result->project_name), written.project_name);
    copy_text(result->partition, sizeof(result->partition), session->partition->label);
    hex_encode(digest, sizeof(digest), result->sha256);
    result->image_size = session->expected_size;

 done:
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_diag.last_error = err;
        if (err == ESP_OK) {
            s_diag.upload_successes++;
            copy_text(s_diag.last_candidate_version, sizeof(s_diag.last_candidate_version), session->candidate.version);
        } else if (err >= GATEWAY_OTA_ERR_BASE && err <= GATEWAY_OTA_ERR_IMAGE_SIZE) {
            s_diag.upload_rejections++;
        } else {
            s_diag.upload_failures++;
        }
        xSemaphoreGive(s_lock);
    }
    memset(digest, 0, sizeof(digest));
    session_release(session, err != ESP_OK);
    return err;
}

void gateway_ota_abort(gateway_ota_session_t *session)
{
    if (session == NULL) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const esp_err_t err = session->last_error != ESP_OK ? session->last_error : ESP_FAIL;
        s_diag.last_error = err;
        if (err >= GATEWAY_OTA_ERR_BASE && err <= GATEWAY_OTA_ERR_IMAGE_SIZE) s_diag.upload_rejections++;
        else s_diag.upload_failures++;
        xSemaphoreGive(s_lock);
    }
    session_release(session, true);
}
