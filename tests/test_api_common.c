#include <assert.h>
#include "api_common.h"

int main(void)
{
    assert(gateway_e164_valid("+491701234567"));
    assert(gateway_e164_valid("+12"));
    assert(!gateway_e164_valid("491701234567"));
    assert(!gateway_e164_valid("+01"));
    assert(!gateway_e164_valid("+1"));
    assert(!gateway_e164_valid("+1234567890123456"));
    assert(!gateway_e164_valid("+49 170"));

    assert(gateway_idempotency_key_valid("request-1234"));
    assert(!gateway_idempotency_key_valid("short"));
    assert(!gateway_idempotency_key_valid("request key with spaces"));

    gateway_rate_limiter_t limiter;
    gateway_rate_limiter_init(&limiter, 2.0, 1.0, 1000);
    assert(gateway_rate_limiter_allow(&limiter, 1.0, 1000));
    assert(gateway_rate_limiter_allow(&limiter, 1.0, 1000));
    assert(!gateway_rate_limiter_allow(&limiter, 1.0, 1000));
    assert(!gateway_rate_limiter_allow(&limiter, 1.0, 1500));
    assert(gateway_rate_limiter_allow(&limiter, 1.0, 2000));
    return 0;
}
