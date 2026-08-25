#pragma once

#include "esp_err.h"
#include "at_engine.h"
#include "modem_manager.h"
#include "usb_modem_transport.h"
#include "sms_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODEM_STATE_BOOT = 0,
    MODEM_STATE_WAIT_USB,
    MODEM_STATE_ENUMERATE,
    MODEM_STATE_FIND_AT_INTERFACE,
    MODEM_STATE_AT_PROBE,
    MODEM_STATE_AT_READY,
    MODEM_STATE_SIM_CHECK,
    MODEM_STATE_NETWORK_REGISTER,
    MODEM_STATE_READY,
    MODEM_STATE_DEGRADED,
    MODEM_STATE_RECOVERY,
} modem_state_t;

esp_err_t modem_core_init(void);
modem_state_t modem_core_state(void);
const char *modem_core_state_name(modem_state_t state);
esp_err_t modem_core_usb_diagnostics(modem_usb_diagnostics_t *out);
esp_err_t modem_core_at_diagnostics(at_engine_diagnostics_t *out);
esp_err_t modem_core_manager_snapshot(modem_manager_snapshot_t *out);
esp_err_t modem_core_submit_sim_pin(const char *pin);
esp_err_t modem_core_restart_modem(void);

/* Durable SMS service API used by REST/MQTT layers. */
esp_err_t modem_core_sms_send(const char *recipient, const char *text, bool delivery_report, uint32_t *message_id);
esp_err_t modem_core_sms_get(uint32_t id, sms_message_t *out);
esp_err_t modem_core_sms_list(uint32_t after_id, sms_message_t *records, size_t max_records, size_t *record_count);
esp_err_t modem_core_sms_delete(uint32_t id);
esp_err_t modem_core_sms_retry_uncertain(uint32_t id);
esp_err_t modem_core_sms_diagnostics(sms_service_diagnostics_t *out);
void modem_core_sms_set_event_replay_watermark(bool protection_enabled, uint32_t watermark);
/** Register one non-blocking observer for SMS lifecycle events (MQTT bridge). */
esp_err_t modem_core_set_sms_event_callback(sms_service_event_callback_t cb, void *user_ctx);

/** Internal service API; management transports must not expose arbitrary AT by default. */
esp_err_t modem_core_at_execute(const at_request_t *request, at_response_t *response);

#ifdef __cplusplus
}
#endif
