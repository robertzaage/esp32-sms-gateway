#include "api_common.h"

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
