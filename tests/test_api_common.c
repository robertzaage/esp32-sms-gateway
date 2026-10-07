#include <assert.h>
#include <string.h>
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
    assert(gateway_at_command_allowed("AT+CPMS?", 192));
    assert(gateway_at_command_allowed("at^portsel?", 192));
    assert(gateway_at_command_allowed("ATI", 192));
    assert(!gateway_at_command_allowed("CPMS?", 192));
    assert(!gateway_at_command_allowed("AT+CMGS=23", 192));
    assert(!gateway_at_command_allowed("at+cmgw", 192));
    assert(!gateway_at_command_allowed("AT\r\nAT+CFUN=0", 192));
    assert(!gateway_at_command_allowed("AT\x1a", 192));
    assert(!gateway_at_command_allowed("AT+CSQ", 6));
    assert(!gateway_at_command_allowed(NULL, 192));

    char prefix[40];
    assert(gateway_at_response_prefix("AT+CPMS?", prefix, sizeof(prefix)) && strcmp(prefix, "+CPMS:") == 0);
    assert(gateway_at_response_prefix("at^portsel=1", prefix, sizeof(prefix)) && strcmp(prefix, "^PORTSEL:") == 0);
    assert(gateway_at_response_prefix("AT+CSQ", prefix, sizeof(prefix)) && strcmp(prefix, "+CSQ:") == 0);
    assert(!gateway_at_response_prefix("ATI", prefix, sizeof(prefix)) && prefix[0] == '\0');
    assert(!gateway_at_response_prefix("AT+", prefix, sizeof(prefix)));
    assert(!gateway_at_response_prefix("AT+CPMS?", prefix, 6));

    return 0;
}
