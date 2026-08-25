#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double capacity;
    double tokens;
    double refill_per_second;
    int64_t last_ms;
} gateway_rate_limiter_t;

bool gateway_e164_valid(const char *value);
bool gateway_idempotency_key_valid(const char *value);
void gateway_rate_limiter_init(gateway_rate_limiter_t *limiter,
                               double capacity,
                               double refill_per_second,
                               int64_t now_ms);
bool gateway_rate_limiter_allow(gateway_rate_limiter_t *limiter,
                                double cost,
                                int64_t now_ms);

#ifdef __cplusplus
}
#endif
