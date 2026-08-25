#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "at_engine.h"
#include "esp_err.h"
#include "modem_status.h"
#include "modem_recovery_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_IDENTITY_MAX 64
#define MODEM_IMEI_MAX 24

typedef enum {
    MODEM_MANAGER_STOPPED = 0,
    MODEM_MANAGER_WAIT_TRANSPORT,
    MODEM_MANAGER_INITIALIZING,
    MODEM_MANAGER_SIM_WAIT,
    MODEM_MANAGER_NETWORK_WAIT,
    MODEM_MANAGER_READY,
    MODEM_MANAGER_DEGRADED,
    MODEM_MANAGER_RECOVERY,
} modem_manager_state_t;

typedef struct {
    modem_manager_state_t state;
    bool transport_ready;
    bool initialized;
    modem_sim_state_t sim;
    modem_registration_t circuit_registration;
    modem_registration_t packet_registration;
    modem_registration_t eps_registration;
    bool registered;
    bool roaming;
    modem_signal_t signal;
    modem_operator_t operator_info;
    char manufacturer[MODEM_IDENTITY_MAX];
    char model[MODEM_IDENTITY_MAX];
    char revision[MODEM_IDENTITY_MAX];
    char imei[MODEM_IMEI_MAX]; /* Private identifier: never publish to MQTT/HA by default. */
    uint32_t initialization_attempts;
    uint32_t status_polls;
    uint32_t consecutive_failures;
    uint32_t recovery_attempts;
    uint32_t hard_resets;
    uint32_t urcs_processed;
    uint32_t urcs_dropped;
    modem_recovery_action_t last_recovery_action;
    at_result_t last_at_result;
    int last_modem_error_code;
    int64_t last_at_success_ms;
    int64_t last_status_update_ms;
    esp_err_t last_error;
} modem_manager_snapshot_t;

typedef esp_err_t (*modem_manager_execute_fn)(void *ctx,
                                               const at_request_t *request,
                                               at_response_t *response);
typedef esp_err_t (*modem_manager_hard_reset_fn)(void *ctx);

typedef struct {
    modem_manager_execute_fn execute;
    modem_manager_hard_reset_fn hard_reset;
    void *ctx;
} modem_manager_transport_t;

typedef void (*modem_manager_event_callback_t)(const modem_manager_snapshot_t *snapshot,
                                                void *user_ctx);

esp_err_t modem_manager_init(const modem_manager_transport_t *transport,
                             modem_manager_event_callback_t event_cb,
                             void *user_ctx);
void modem_manager_notify_transport_ready(void);
void modem_manager_notify_transport_lost(void);

/** AT engine URC callback. This function is non-blocking and only queues the line. */
void modem_manager_handle_urc(const char *line, void *user_ctx);

esp_err_t modem_manager_get_snapshot(modem_manager_snapshot_t *out);

/** Submit a volatile SIM PIN. It is validated, queued and zeroized after use; it is not persisted. */
esp_err_t modem_manager_submit_sim_pin(const char *pin);
/** Queue a controlled modem restart. Uses VBUS power-cycle when available, else CFUN reset. */
esp_err_t modem_manager_request_restart(void);

const char *modem_manager_state_name(modem_manager_state_t state);

#ifdef __cplusplus
}
#endif
