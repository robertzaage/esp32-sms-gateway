#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gateway_security.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_IDEMPOTENCY_MAX_RECORDS 32

typedef enum {
    GATEWAY_IDEMPOTENCY_MISS = 0,
    GATEWAY_IDEMPOTENCY_REPLAY,
    GATEWAY_IDEMPOTENCY_CONFLICT,
} gateway_idempotency_result_t;

typedef struct {
    uint16_t pending_records;
    uint16_t finalized_records;
    uint16_t free_records;
} gateway_idempotency_diagnostics_t;

esp_err_t gateway_idempotency_init(void);
esp_err_t gateway_idempotency_sms_fingerprint(const char *recipient,
                                              const char *text,
                                              bool delivery_report,
                                              uint8_t out[GATEWAY_SHA256_LEN]);
esp_err_t gateway_idempotency_lookup(const char *key,
                                     const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                     gateway_idempotency_result_t *result,
                                     uint32_t *message_id);
esp_err_t gateway_idempotency_claim(const char *key,
                                    const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                    gateway_idempotency_result_t *result,
                                    uint32_t *message_id);
esp_err_t gateway_idempotency_finalize(const char *key,
                                       const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                       uint32_t message_id);
esp_err_t gateway_idempotency_release_pending(const char *key,
                                              const uint8_t request_hash[GATEWAY_SHA256_LEN]);
esp_err_t gateway_idempotency_remember(const char *key,
                                       const uint8_t request_hash[GATEWAY_SHA256_LEN],
                                       uint32_t message_id);
esp_err_t gateway_idempotency_get_diagnostics(gateway_idempotency_diagnostics_t *out);
/** Explicit operator recovery. Clearing pending reservations can permit a duplicate SMS. */
esp_err_t gateway_idempotency_clear_pending(uint32_t *cleared);

#ifdef __cplusplus
}
#endif
