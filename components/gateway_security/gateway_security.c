#include "gateway_security.h"

#include <string.h>
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"

#define SECURITY_NAMESPACE "gateway_sec"
#define TOKEN_HASH_KEY "api_hash"

static uint8_t s_token_hash[GATEWAY_SHA256_LEN];
static bool s_initialized;

void gateway_security_wipe(void *data, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (p != NULL && len-- > 0) {
        *p++ = 0;
    }
}

esp_err_t gateway_security_sha256(const void *data, size_t len, uint8_t out[GATEWAY_SHA256_LEN])
{
    if ((data == NULL && len != 0) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    size_t written = 0;
    const psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256,
                                                 (const uint8_t *)data,
                                                 len,
                                                 out,
                                                 GATEWAY_SHA256_LEN,
                                                 &written);
    return status == PSA_SUCCESS && written == GATEWAY_SHA256_LEN ? ESP_OK : ESP_FAIL;
}

static bool constant_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void hex_encode(const uint8_t *src, size_t src_len, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < src_len; ++i) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    dst[src_len * 2] = '\0';
}

esp_err_t gateway_security_init(char *bootstrap_token, size_t bootstrap_capacity, bool *generated)
{
    if (generated == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *generated = false;
    if (bootstrap_token != NULL && bootstrap_capacity > 0) {
        bootstrap_token[0] = '\0';
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SECURITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_token_hash);
    err = nvs_get_blob(handle, TOKEN_HASH_KEY, s_token_hash, &size);
    if (err == ESP_OK && size == sizeof(s_token_hash)) {
        nvs_close(handle);
        s_initialized = true;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    if (bootstrap_token == NULL || bootstrap_capacity < GATEWAY_API_TOKEN_HEX_LEN + 1) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t random_bytes[GATEWAY_API_TOKEN_HEX_LEN / 2];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    hex_encode(random_bytes, sizeof(random_bytes), bootstrap_token);
    gateway_security_wipe(random_bytes, sizeof(random_bytes));

    err = gateway_security_sha256(bootstrap_token, strlen(bootstrap_token), s_token_hash);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, TOKEN_HASH_KEY, s_token_hash, sizeof(s_token_hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        gateway_security_wipe(bootstrap_token, bootstrap_capacity);
        gateway_security_wipe(s_token_hash, sizeof(s_token_hash));
        return err;
    }
    s_initialized = true;
    *generated = true;
    return ESP_OK;
}

bool gateway_security_validate_bearer(const char *token)
{
    if (!s_initialized || token == NULL) {
        return false;
    }
    const size_t len = strlen(token);
    if (len < 32 || len > 128) {
        return false;
    }
    uint8_t candidate[GATEWAY_SHA256_LEN];
    if (gateway_security_sha256(token, len, candidate) != ESP_OK) {
        return false;
    }
    const bool valid = constant_equal(candidate, s_token_hash, sizeof(candidate));
    gateway_security_wipe(candidate, sizeof(candidate));
    return valid;
}
