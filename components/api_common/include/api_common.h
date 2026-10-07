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

/**
 * True for a single diagnostic AT command: starts with "AT" (any case),
 * shorter than max_length, no CR/LF/Ctrl-Z, and not a prompt command
 * (AT+CMGS, AT+CMGW) that would leave the modem waiting for a PDU.
 */
bool gateway_at_command_allowed(const char *command, size_t max_length);
/**
 * Response prefix of an extended AT command, e.g. "+CPMS:" for "AT+CPMS?" or
 * "^PORTSEL:" for "AT^PORTSEL=1". Writes "" and returns false for basic
 * commands such as "ATI" or when out is too small.
 */
bool gateway_at_response_prefix(const char *command, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
