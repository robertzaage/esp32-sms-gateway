#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_engine.h"
#include "esp_err.h"
#include "sms_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*sms_service_execute_fn)(void *ctx,
                                            const at_request_t *request,
                                            at_response_t *response);
typedef bool (*sms_service_ready_fn)(void *ctx);

typedef struct {
    sms_service_execute_fn execute;
    sms_service_ready_fn is_ready;
    void *ctx;
} sms_service_transport_t;

typedef enum {
    SMS_SERVICE_EVENT_RECEIVED = 0,
    SMS_SERVICE_EVENT_PARTIAL_RECEIVED,
    SMS_SERVICE_EVENT_SENT,
    SMS_SERVICE_EVENT_DELIVERED,
    SMS_SERVICE_EVENT_FAILED,
    SMS_SERVICE_EVENT_UNSUPPORTED_RECEIVED,
    SMS_SERVICE_EVENT_UNCERTAIN,
} sms_service_event_t;

typedef void (*sms_service_event_callback_t)(sms_service_event_t event,
                                              const sms_message_t *message,
                                              void *user_ctx);

typedef struct {
    bool initialized;
    bool modem_ready;
    bool pdu_mode_configured;
    uint32_t inbound_pdus;
    uint32_t inbound_messages;
    uint32_t inbound_parts;
    uint32_t duplicate_parts;
    uint32_t duplicate_messages;
    uint32_t decode_failures;
    uint32_t unsupported_pdus;
    uint32_t store_failures;
    uint32_t outbound_queued;
    uint32_t outbound_segments_sent;
    uint32_t outbound_uncertain;
    uint32_t delivery_reports;
    uint32_t urcs_dropped;
    uint32_t store_pruned_records;
    uint32_t store_capacity_failures;
    uint16_t store_used_records;
    uint16_t store_free_records;
    int last_cms_error;
    esp_err_t last_error;
} sms_service_diagnostics_t;

esp_err_t sms_service_init(const sms_service_transport_t *transport,
                           sms_service_event_callback_t event_cb,
                           void *user_ctx);
void sms_service_notify_modem_ready(void);
void sms_service_notify_modem_lost(void);

/** Non-blocking callback for AT URC dispatch. */
void sms_service_handle_urc(const char *line, void *user_ctx);

/** Persist and queue an outbound SMS. Returns after durable acceptance, not network send. */
esp_err_t sms_service_send(const char *recipient,
                           const char *utf8_text,
                           bool request_delivery_report,
                           uint32_t *message_id);

esp_err_t sms_service_get(uint32_t id, sms_message_t *out);
esp_err_t sms_service_list(uint32_t after_id,
                           sms_message_t *records,
                           size_t max_records,
                           size_t *record_count);

/** Delete a stored message. Sending/in-flight messages cannot be deleted. */
esp_err_t sms_service_delete(uint32_t id);

/**
 * Explicitly retry an uncertain outbound message. This may duplicate the
 * segment whose acknowledgement was lost, so callers must opt in deliberately.
 */
esp_err_t sms_service_retry_uncertain(uint32_t id);

esp_err_t sms_service_get_diagnostics(sms_service_diagnostics_t *out);
void sms_service_set_event_replay_watermark(bool protection_enabled, uint32_t watermark);

#ifdef __cplusplus
}
#endif
