#include "api_common.h"

#include <ctype.h>
#include <string.h>

bool gateway_e164_valid(const char *value)
{
    if (value == NULL || value[0] != '+') {
        return false;
    }
    size_t digits = 0;
    for (size_t i = 1; value[i] != '\0'; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        if (i == 1 && value[i] == '0') {
            return false;
        }
        ++digits;
        if (digits > 15) {
            return false;
        }
    }
    return digits >= 2;
}

bool gateway_idempotency_key_valid(const char *value)
{
    if (value == NULL) {
        return false;
    }
    const size_t len = strlen(value);
    if (len < 8 || len > 128) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void gateway_rate_limiter_init(gateway_rate_limiter_t *limiter,
                               double capacity,
                               double refill_per_second,
                               int64_t now_ms)
{
    if (limiter == NULL) {
        return;
    }
    limiter->capacity = capacity > 0.0 ? capacity : 1.0;
    limiter->tokens = limiter->capacity;
    limiter->refill_per_second = refill_per_second > 0.0 ? refill_per_second : 0.0;
    limiter->last_ms = now_ms;
}

bool gateway_rate_limiter_allow(gateway_rate_limiter_t *limiter,
                                double cost,
                                int64_t now_ms)
{
    if (limiter == NULL || cost <= 0.0) {
        return false;
    }
    if (now_ms > limiter->last_ms && limiter->refill_per_second > 0.0) {
        const double elapsed = (double)(now_ms - limiter->last_ms) / 1000.0;
        limiter->tokens += elapsed * limiter->refill_per_second;
        if (limiter->tokens > limiter->capacity) {
            limiter->tokens = limiter->capacity;
        }
        limiter->last_ms = now_ms;
    }
    if (limiter->tokens + 1e-9 < cost) {
        return false;
    }
    limiter->tokens -= cost;
    return true;
}

static bool starts_with_ignore_case(const char *value, const char *prefix)
{
    for (; *prefix != '\0'; ++value, ++prefix) {
        if (toupper((unsigned char)*value) != toupper((unsigned char)*prefix)) {
            return false;
        }
    }
    return true;
}

bool gateway_at_command_allowed(const char *command, size_t max_length)
{
    if (command == NULL || !starts_with_ignore_case(command, "AT")) {
        return false;
    }
    const size_t length = strlen(command);
    if (length >= max_length || strpbrk(command, "\r\n\x1a") != NULL) {
        return false;
    }
    return !starts_with_ignore_case(command, "AT+CMGS") && !starts_with_ignore_case(command, "AT+CMGW");
}

bool gateway_at_response_prefix(const char *command, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (command == NULL || !starts_with_ignore_case(command, "AT") || (command[2] != '+' && command[2] != '^')) {
        return false;
    }
    const size_t length = strcspn(command + 2, "=?");
    if (length < 2 || length + 2 > out_size) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = (char)toupper((unsigned char)command[2 + i]);
    }
    out[length] = ':';
    out[length + 1] = '\0';
    return true;
}
