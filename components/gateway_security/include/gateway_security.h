#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_API_TOKEN_HEX_LEN 64
#define GATEWAY_SHA256_LEN 32

/**
 * Initialize security state. If a token is generated, it is returned once in
 * bootstrap_token and generated is set true. Only its SHA-256 digest is stored.
 */
esp_err_t gateway_security_init(char *bootstrap_token, size_t bootstrap_capacity, bool *generated);
bool gateway_security_validate_bearer(const char *token);
esp_err_t gateway_security_sha256(const void *data, size_t len, uint8_t out[GATEWAY_SHA256_LEN]);
void gateway_security_wipe(void *data, size_t len);

#ifdef __cplusplus
}
#endif
