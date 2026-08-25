#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_ENGINE_MAX_COMMAND_LENGTH 192
#define AT_ENGINE_MAX_EXPECTED_PREFIXES 8
#define AT_ENGINE_MAX_RESPONSE_BYTES 2048
#define AT_ENGINE_MAX_RESPONSE_LINES 24
#define AT_ENGINE_MAX_URC_LENGTH 512
#define AT_ENGINE_MAX_FINAL_LENGTH 128

typedef enum {
    AT_RESULT_OK = 0,
    AT_RESULT_ERROR,
    AT_RESULT_CME_ERROR,
    AT_RESULT_CMS_ERROR,
    AT_RESULT_TIMEOUT,
    AT_RESULT_CANCELED,
    AT_RESULT_TRANSPORT_UNAVAILABLE,
    AT_RESULT_TRANSPORT_ERROR,
    AT_RESULT_RX_OVERFLOW,
    AT_RESULT_PROTOCOL_ERROR,
} at_result_t;

typedef enum {
    AT_RETRY_NONE = 0,
    AT_RETRY_ON_TIMEOUT = 1U << 0,
    AT_RETRY_ON_ERROR = 1U << 1,
    AT_RETRY_ON_CME_ERROR = 1U << 2,
    AT_RETRY_ON_CMS_ERROR = 1U << 3,
    AT_RETRY_ON_TRANSPORT_ERROR = 1U << 4,
} at_retry_policy_t;

typedef struct {
    const char *command; /* Without the terminating CR. */
    const char *const *expected_prefixes;
    size_t expected_prefix_count;
    uint32_t timeout_ms;
    uint8_t max_attempts; /* 0 means one attempt. */
    uint32_t retry_delay_ms;
    uint32_t retry_policy;

    /*
     * Prompt transactions (for example AT+CMGS) are atomic: when
     * wait_for_prompt is true, prompt_payload is mandatory. The engine waits
     * for '>', writes the payload exactly as supplied, then continues waiting
     * for a final result before releasing the serialized modem channel. The
     * caller includes Ctrl-Z (0x1a) when required by the modem command.
     */
    bool wait_for_prompt;
    const uint8_t *prompt_payload;
    size_t prompt_payload_len;
} at_request_t;

typedef struct {
    at_result_t result;
    int error_code;
    uint8_t attempts;
    bool truncated;
    char final_line[AT_ENGINE_MAX_FINAL_LENGTH];
    size_t data_length;
    size_t line_count;
    uint16_t line_offsets[AT_ENGINE_MAX_RESPONSE_LINES];
    char data[AT_ENGINE_MAX_RESPONSE_BYTES];
} at_response_t;

typedef esp_err_t (*at_transport_write_fn)(void *ctx,
                                           const uint8_t *data,
                                           size_t len,
                                           uint32_t timeout_ms);
typedef bool (*at_transport_ready_fn)(void *ctx);

typedef struct {
    at_transport_write_fn write;
    at_transport_ready_fn is_ready;
    void *ctx;
    uint32_t write_timeout_ms;
} at_transport_t;

typedef void (*at_urc_callback_t)(const char *line, void *user_ctx);

typedef struct {
    uint32_t commands_submitted;
    uint32_t commands_completed;
    uint32_t commands_retried;
    uint32_t timeouts;
    uint32_t cancellations;
    uint32_t transport_errors;
    uint32_t rx_overflows;
    uint32_t parser_overflows;
    uint32_t urcs_dispatched;
    uint32_t urcs_dropped;
} at_engine_diagnostics_t;

esp_err_t at_engine_init(const at_transport_t *transport);

/**
 * Execute one serialized AT transaction. Safe to call concurrently from
 * different tasks; the engine is the only transport writer.
 */
esp_err_t at_engine_execute(const at_request_t *request, at_response_t *response);

/** Non-blocking ingress used by the modem transport receive callback. */
void at_engine_feed_rx(const uint8_t *data, size_t data_len);

/** Wake and abort the active transaction because its transport disappeared. */
void at_engine_notify_transport_lost(void);

/** Cancel the currently executing transaction. Queued transactions remain. */
esp_err_t at_engine_cancel_current(void);

/** Set the asynchronous URC consumer. It runs in a dedicated dispatcher task. */
esp_err_t at_engine_set_urc_callback(at_urc_callback_t callback, void *user_ctx);

esp_err_t at_engine_get_diagnostics(at_engine_diagnostics_t *out);
const char *at_engine_result_name(at_result_t result);

/** Return a captured response line by zero-based index, or NULL. */
const char *at_response_line(const at_response_t *response, size_t index);

#ifdef __cplusplus
}
#endif
