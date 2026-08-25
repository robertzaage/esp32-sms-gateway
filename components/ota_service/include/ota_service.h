#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_OTA_SHA256_HEX_LEN 64
#define GATEWAY_OTA_ERR_BASE                 0x7A00
#define GATEWAY_OTA_ERR_HASH_MISMATCH       (GATEWAY_OTA_ERR_BASE + 1)
#define GATEWAY_OTA_ERR_PROJECT_MISMATCH    (GATEWAY_OTA_ERR_BASE + 2)
#define GATEWAY_OTA_ERR_VERSION_FORMAT      (GATEWAY_OTA_ERR_BASE + 3)
#define GATEWAY_OTA_ERR_REINSTALL_REJECTED  (GATEWAY_OTA_ERR_BASE + 4)
#define GATEWAY_OTA_ERR_DOWNGRADE_REJECTED  (GATEWAY_OTA_ERR_BASE + 5)
#define GATEWAY_OTA_ERR_SECURE_VERSION      (GATEWAY_OTA_ERR_BASE + 6)
#define GATEWAY_OTA_ERR_PENDING_VERIFY      (GATEWAY_OTA_ERR_BASE + 7)
#define GATEWAY_OTA_ERR_IMAGE_SIZE          (GATEWAY_OTA_ERR_BASE + 8)

typedef struct gateway_ota_session gateway_ota_session_t;

typedef struct {
    size_t image_size;
    const char *sha256_hex;
    bool allow_reinstall;
    bool allow_downgrade;
} gateway_ota_request_t;

typedef struct {
    char version[32];
    char project_name[32];
    char partition[17];
    char sha256[65];
    size_t image_size;
} gateway_ota_result_t;

typedef struct {
    bool pending_verify;
    bool confirmation_scheduled;
    bool update_in_progress;
    uint32_t upload_attempts;
    uint32_t upload_successes;
    uint32_t upload_rejections;
    uint32_t upload_failures;
    int last_error;
    char running_version[32];
    char running_partition[17];
    char boot_version[32];
    char boot_partition[17];
    char last_candidate_version[32];
} gateway_ota_diagnostics_t;

esp_err_t gateway_ota_service_init(void);
/* Call after all critical application services have initialized successfully. */
esp_err_t gateway_ota_mark_services_ready(void);
esp_err_t gateway_ota_get_diagnostics(gateway_ota_diagnostics_t *out);

esp_err_t gateway_ota_begin(const gateway_ota_request_t *request, gateway_ota_session_t **out_session);
esp_err_t gateway_ota_write(gateway_ota_session_t *session, const void *data, size_t len);
/* Consumes/frees session on both success and failure. */
esp_err_t gateway_ota_finish(gateway_ota_session_t *session, gateway_ota_result_t *result);
/* Consumes/frees session. Safe after a failed write. */
void gateway_ota_abort(gateway_ota_session_t *session);

#ifdef __cplusplus
}
#endif
