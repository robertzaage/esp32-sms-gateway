#include "sdkconfig.h"
#include "modem_manager.h"
#include "modem_recovery_policy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MODEM_MANAGER_QUEUE_DEPTH 16
#define MODEM_MANAGER_TASK_STACK 7168
#define MODEM_MANAGER_TASK_PRIORITY 10
#define MODEM_MANAGER_URC_MAX AT_ENGINE_MAX_URC_LENGTH
#define MODEM_MANAGER_PIN_MAX 8
#define MODEM_MANAGER_DEFAULT_TIMEOUT_MS 3000
#define MODEM_MANAGER_OPERATOR_TIMEOUT_MS 10000

static const char *TAG = "modem_manager";

typedef enum {
    MANAGER_EVENT_TRANSPORT_READY = 0,
    MANAGER_EVENT_TRANSPORT_LOST,
    MANAGER_EVENT_URC,
    MANAGER_EVENT_SIM_PIN,
    MANAGER_EVENT_MANUAL_RESTART,
} manager_event_id_t;

typedef struct {
    manager_event_id_t id;
    union {
        char urc[MODEM_MANAGER_URC_MAX];
        char pin[MODEM_MANAGER_PIN_MAX + 1];
    } data;
} manager_event_t;

typedef struct {
    bool initialized;
    modem_manager_transport_t transport;
    modem_manager_event_callback_t event_cb;
    void *event_user_ctx;
    QueueHandle_t queue;
    TaskHandle_t task;
    modem_manager_snapshot_t snapshot;
    uint32_t recoveries_since_hard_reset;
    uint32_t successful_poll_cycles;
    bool reinitialize_pending;
    int64_t next_poll_ms;
    int64_t next_operator_poll_ms;
} manager_state_t;

static manager_state_t s_manager;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void snapshot_copy(modem_manager_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_manager.snapshot;
    portEXIT_CRITICAL(&s_lock);
}

static void emit_snapshot(void)
{
    if (s_manager.event_cb == NULL) {
        return;
    }
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    s_manager.event_cb(&snapshot, s_manager.event_user_ctx);
}

static void snapshot_set_state(modem_manager_state_t state)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_manager.snapshot.state != state) {
        s_manager.snapshot.state = state;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        ESP_LOGI(TAG, "state=%s", modem_manager_state_name(state));
        emit_snapshot();
    }
}

static void snapshot_record_success(const at_response_t *response)
{
    const int64_t timestamp = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.consecutive_failures = 0;
    s_manager.snapshot.last_at_success_ms = timestamp;
    s_manager.snapshot.last_error = ESP_OK;
    s_manager.snapshot.last_at_result = AT_RESULT_OK;
    s_manager.snapshot.last_modem_error_code = response != NULL ? response->error_code : -1;
    portEXIT_CRITICAL(&s_lock);
}

static void snapshot_record_failure(esp_err_t error, at_result_t result, int modem_error_code)
{
    portENTER_CRITICAL(&s_lock);
    ++s_manager.snapshot.consecutive_failures;
    s_manager.snapshot.last_error = error;
    s_manager.snapshot.last_at_result = result;
    s_manager.snapshot.last_modem_error_code = modem_error_code;
    portEXIT_CRITICAL(&s_lock);
}

static void copy_identity(char *target, size_t target_size, const char *value)
{
    if (target == NULL || target_size == 0 || value == NULL) {
        return;
    }
    size_t out = 0;
    while (*value != '\0' && out + 1 < target_size) {
        const unsigned char ch = (unsigned char)*value++;
        if (ch >= 0x20 && ch <= 0x7e) {
            target[out++] = (char)ch;
        }
    }
    target[out] = '\0';
}

static bool execute_command(const char *command,
                            const char *const *prefixes,
                            size_t prefix_count,
                            uint32_t timeout_ms,
                            at_response_t *response)
{
    at_request_t request = {
        .command = command,
        .expected_prefixes = prefixes,
        .expected_prefix_count = prefix_count,
        .timeout_ms = timeout_ms == 0 ? MODEM_MANAGER_DEFAULT_TIMEOUT_MS : timeout_ms,
        .max_attempts = 2,
        .retry_delay_ms = 150,
        .retry_policy = AT_RETRY_ON_TIMEOUT | AT_RETRY_ON_TRANSPORT_ERROR,
    };
    at_response_t local_response;
    if (response == NULL) {
        response = &local_response;
    }

    const esp_err_t err = s_manager.transport.execute(s_manager.transport.ctx, &request, response);
    if (err != ESP_OK) {
        snapshot_record_failure(err, AT_RESULT_TRANSPORT_ERROR, -1);
        return false;
    }
    if (response->result != AT_RESULT_OK) {
        const esp_err_t mapped = response->result == AT_RESULT_TRANSPORT_UNAVAILABLE ||
                                         response->result == AT_RESULT_TRANSPORT_ERROR
                                     ? ESP_ERR_INVALID_STATE
                                     : ESP_FAIL;
        snapshot_record_failure(mapped, response->result, response->error_code);
        ESP_LOGW(TAG, "AT transaction failed result=%s code=%d",
                 at_engine_result_name(response->result), response->error_code);
        return false;
    }

    snapshot_record_success(response);
    return true;
}

static bool execute_optional(const char *command,
                             const char *const *prefixes,
                             size_t prefix_count,
                             uint32_t timeout_ms,
                             at_response_t *response)
{
    uint32_t failures_before;
    esp_err_t error_before;
    at_result_t at_result_before;
    int modem_error_before;
    portENTER_CRITICAL(&s_lock);
    failures_before = s_manager.snapshot.consecutive_failures;
    error_before = s_manager.snapshot.last_error;
    at_result_before = s_manager.snapshot.last_at_result;
    modem_error_before = s_manager.snapshot.last_modem_error_code;
    portEXIT_CRITICAL(&s_lock);
    const bool ok = execute_command(command, prefixes, prefix_count, timeout_ms, response);
    if (!ok) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.consecutive_failures = failures_before;
        s_manager.snapshot.last_error = error_before;
        s_manager.snapshot.last_at_result = at_result_before;
        s_manager.snapshot.last_modem_error_code = modem_error_before;
        portEXIT_CRITICAL(&s_lock);
    }
    return ok;
}

static bool response_parse_sim(const at_response_t *response)
{
    for (size_t i = 0; i < response->line_count; ++i) {
        const char *line = at_response_line(response, i);
        modem_sim_state_t sim;
        if (modem_status_parse_cpin(line, &sim)) {
            portENTER_CRITICAL(&s_lock);
            s_manager.snapshot.sim = sim;
            s_manager.snapshot.last_status_update_ms = now_ms();
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
    }
    return false;
}

static bool response_parse_signal(const at_response_t *response)
{
    for (size_t i = 0; i < response->line_count; ++i) {
        modem_signal_t signal;
        if (modem_status_parse_csq(at_response_line(response, i), &signal)) {
            portENTER_CRITICAL(&s_lock);
            s_manager.snapshot.signal = signal;
            s_manager.snapshot.last_status_update_ms = now_ms();
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
    }
    return false;
}

static bool response_parse_registration(const at_response_t *response,
                                        const char *prefix,
                                        modem_registration_t *target)
{
    for (size_t i = 0; i < response->line_count; ++i) {
        modem_registration_t registration;
        if (modem_status_parse_registration(at_response_line(response, i), prefix, &registration)) {
            portENTER_CRITICAL(&s_lock);
            *target = registration;
            s_manager.snapshot.last_status_update_ms = now_ms();
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
    }
    return false;
}

static bool response_parse_operator(const at_response_t *response)
{
    for (size_t i = 0; i < response->line_count; ++i) {
        modem_operator_t operator_info;
        if (modem_status_parse_operator(at_response_line(response, i), &operator_info)) {
            portENTER_CRITICAL(&s_lock);
            s_manager.snapshot.operator_info = operator_info;
            s_manager.snapshot.last_status_update_ms = now_ms();
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
    }
    return false;
}

static void recompute_registration(void)
{
    modem_registration_t eps;
    modem_registration_t packet;
    modem_registration_t circuit;
    portENTER_CRITICAL(&s_lock);
    eps = s_manager.snapshot.eps_registration;
    packet = s_manager.snapshot.packet_registration;
    circuit = s_manager.snapshot.circuit_registration;
    portEXIT_CRITICAL(&s_lock);

    const modem_registration_status_t statuses[] = {eps.status, packet.status, circuit.status};
    bool registered = false;
    bool roaming = false;
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        if (modem_registration_is_registered(statuses[i])) {
            registered = true;
            roaming = modem_registration_is_roaming(statuses[i]);
            break;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.registered = registered;
    s_manager.snapshot.roaming = roaming;
    portEXIT_CRITICAL(&s_lock);
}

static bool query_sim(void)
{
    static const char *const prefixes[] = {"+CPIN:"};
    at_response_t response;
    return execute_command("AT+CPIN?", prefixes, 1, 3000, &response) && response_parse_sim(&response);
}

static bool query_signal(void)
{
    static const char *const prefixes[] = {"+CSQ:"};
    at_response_t response;
    return execute_command("AT+CSQ", prefixes, 1, 3000, &response) && response_parse_signal(&response);
}

static bool query_registration_one(const char *command,
                                   const char *prefix,
                                   modem_registration_t *target,
                                   bool optional)
{
    const char *prefixes[] = {prefix};
    at_response_t response;
    const bool ok = optional
                        ? execute_optional(command, prefixes, 1, 3000, &response)
                        : execute_command(command, prefixes, 1, 3000, &response);
    return ok && response_parse_registration(&response, prefix, target);
}

static bool query_registration(void)
{
    bool any = false;
    any |= query_registration_one("AT+CREG?", "+CREG:", &s_manager.snapshot.circuit_registration, false);
    any |= query_registration_one("AT+CGREG?", "+CGREG:", &s_manager.snapshot.packet_registration, true);
    any |= query_registration_one("AT+CEREG?", "+CEREG:", &s_manager.snapshot.eps_registration, true);
    recompute_registration();
    return any;
}

static bool query_operator(void)
{
    static const char *const prefixes[] = {"+COPS:"};
    at_response_t response;
    return execute_optional("AT+COPS?", prefixes, 1, MODEM_MANAGER_OPERATOR_TIMEOUT_MS, &response) &&
           response_parse_operator(&response);
}

static void configure_registration_urc(const char *extended, const char *basic)
{
    if (!execute_optional(extended, NULL, 0, 3000, NULL)) {
        (void)execute_optional(basic, NULL, 0, 3000, NULL);
    }
}

static void query_identity(const char *command, char *target, size_t target_size)
{
    at_response_t response;
    if (!execute_optional(command, NULL, 0, 3000, &response)) {
        return;
    }
    for (size_t i = 0; i < response.line_count; ++i) {
        const char *line = at_response_line(&response, i);
        if (line != NULL && line[0] != '\0') {
            portENTER_CRITICAL(&s_lock);
            copy_identity(target, target_size, line);
            portEXIT_CRITICAL(&s_lock);
            break;
        }
    }
}

static bool initialize_session(void)
{
    snapshot_set_state(MODEM_MANAGER_INITIALIZING);
    portENTER_CRITICAL(&s_lock);
    ++s_manager.snapshot.initialization_attempts;
    s_manager.snapshot.initialized = false;
    portEXIT_CRITICAL(&s_lock);

    if (!execute_command("ATE0", NULL, 0, 3000, NULL)) {
        return false;
    }
    if (!execute_command("AT+CMEE=2", NULL, 0, 3000, NULL)) {
        return false;
    }

    query_identity("AT+CGMI", s_manager.snapshot.manufacturer, sizeof(s_manager.snapshot.manufacturer));
    query_identity("AT+CGMM", s_manager.snapshot.model, sizeof(s_manager.snapshot.model));
    query_identity("AT+CGMR", s_manager.snapshot.revision, sizeof(s_manager.snapshot.revision));
    query_identity("AT+CGSN", s_manager.snapshot.imei, sizeof(s_manager.snapshot.imei));

    if (!query_sim()) {
        return false;
    }

    configure_registration_urc("AT+CREG=2", "AT+CREG=1");
    configure_registration_urc("AT+CGREG=2", "AT+CGREG=1");
    configure_registration_urc("AT+CEREG=2", "AT+CEREG=1");

    portENTER_CRITICAL(&s_lock);
    const modem_sim_state_t sim = s_manager.snapshot.sim;
    s_manager.snapshot.initialized = true;
    portEXIT_CRITICAL(&s_lock);

    if (sim != MODEM_SIM_READY) {
        snapshot_set_state(MODEM_MANAGER_SIM_WAIT);
        s_manager.next_poll_ms = now_ms() + CONFIG_GATEWAY_MODEM_POLL_WAIT_MS;
        emit_snapshot();
        return true;
    }

    (void)query_registration();
    (void)query_signal();
    (void)query_operator();
    s_manager.next_operator_poll_ms = now_ms() + 300000;
    recompute_registration();

    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    snapshot_set_state(snapshot.registered ? MODEM_MANAGER_READY : MODEM_MANAGER_NETWORK_WAIT);
    s_manager.next_poll_ms = now_ms() +
                             (snapshot.registered ? CONFIG_GATEWAY_MODEM_POLL_READY_MS
                                                  : CONFIG_GATEWAY_MODEM_POLL_WAIT_MS);
    emit_snapshot();
    return true;
}

static void update_state_after_poll(void)
{
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    if (snapshot.sim != MODEM_SIM_READY) {
        snapshot_set_state(MODEM_MANAGER_SIM_WAIT);
    } else if (snapshot.registered) {
        snapshot_set_state(MODEM_MANAGER_READY);
    } else {
        snapshot_set_state(MODEM_MANAGER_NETWORK_WAIT);
    }
}

static bool poll_status(void)
{
    portENTER_CRITICAL(&s_lock);
    ++s_manager.snapshot.status_polls;
    const modem_sim_state_t sim = s_manager.snapshot.sim;
    portEXIT_CRITICAL(&s_lock);

    if (sim != MODEM_SIM_READY) {
        const bool ok = query_sim();
        if (ok) {
            modem_manager_snapshot_t snapshot;
            snapshot_copy(&snapshot);
            if (snapshot.sim == MODEM_SIM_READY) {
                s_manager.reinitialize_pending = true;
            }
        }
        update_state_after_poll();
        emit_snapshot();
        return ok;
    }

    const bool reg_ok = query_registration();
    const bool signal_ok = query_signal();
    if (now_ms() >= s_manager.next_operator_poll_ms) {
        (void)query_operator();
        s_manager.next_operator_poll_ms = now_ms() + 300000;
    }
    update_state_after_poll();
    emit_snapshot();
    return reg_ok && signal_ok;
}

static void handle_urc_line(const char *line)
{
    bool changed = false;
    modem_sim_state_t sim;
    modem_signal_t signal;
    modem_registration_t registration;

    if (modem_status_parse_cpin(line, &sim)) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.sim = sim;
        s_manager.snapshot.last_status_update_ms = now_ms();
        ++s_manager.snapshot.urcs_processed;
        portEXIT_CRITICAL(&s_lock);
        if (sim == MODEM_SIM_READY) {
            s_manager.reinitialize_pending = true;
            s_manager.next_poll_ms = now_ms();
        }
        changed = true;
    } else if (modem_status_parse_csq(line, &signal)) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.signal = signal;
        s_manager.snapshot.last_status_update_ms = now_ms();
        ++s_manager.snapshot.urcs_processed;
        portEXIT_CRITICAL(&s_lock);
        changed = true;
    } else if (modem_status_parse_registration(line, "+CREG:", &registration)) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.circuit_registration = registration;
        s_manager.snapshot.last_status_update_ms = now_ms();
        ++s_manager.snapshot.urcs_processed;
        portEXIT_CRITICAL(&s_lock);
        s_manager.next_operator_poll_ms = 0;
        s_manager.next_poll_ms = now_ms();
        changed = true;
    } else if (modem_status_parse_registration(line, "+CGREG:", &registration)) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.packet_registration = registration;
        s_manager.snapshot.last_status_update_ms = now_ms();
        ++s_manager.snapshot.urcs_processed;
        portEXIT_CRITICAL(&s_lock);
        s_manager.next_operator_poll_ms = 0;
        s_manager.next_poll_ms = now_ms();
        changed = true;
    } else if (modem_status_parse_registration(line, "+CEREG:", &registration)) {
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.eps_registration = registration;
        s_manager.snapshot.last_status_update_ms = now_ms();
        ++s_manager.snapshot.urcs_processed;
        portEXIT_CRITICAL(&s_lock);
        s_manager.next_operator_poll_ms = 0;
        s_manager.next_poll_ms = now_ms();
        changed = true;
    }

    if (changed) {
        recompute_registration();
        update_state_after_poll();
        emit_snapshot();
    }
}

static void schedule_recovery(void)
{
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    const modem_recovery_action_t action = modem_recovery_policy_next(
        snapshot.consecutive_failures,
        CONFIG_GATEWAY_MODEM_FAILURES_BEFORE_RECOVERY,
        CONFIG_GATEWAY_MODEM_HARD_RESET_AFTER_RECOVERIES,
        &s_manager.recoveries_since_hard_reset);
    if (action == MODEM_RECOVERY_NONE) {
        return;
    }

    snapshot_set_state(MODEM_MANAGER_RECOVERY);
    portENTER_CRITICAL(&s_lock);
    ++s_manager.snapshot.recovery_attempts;
    s_manager.snapshot.last_recovery_action = action;
    portEXIT_CRITICAL(&s_lock);

    if (action == MODEM_RECOVERY_REINITIALIZE) {
        ESP_LOGW(TAG, "recovery: reinitializing modem AT profile");
        s_manager.reinitialize_pending = true;
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.consecutive_failures = 0;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    if (action == MODEM_RECOVERY_FUNCTIONAL_RESET) {
        ESP_LOGW(TAG, "recovery: requesting modem functional reset");
        (void)execute_optional("AT+CFUN=1,1", NULL, 0, 5000, NULL);
        portENTER_CRITICAL(&s_lock);
        s_manager.snapshot.initialized = false;
        s_manager.snapshot.consecutive_failures = 0;
        portEXIT_CRITICAL(&s_lock);
        s_manager.reinitialize_pending = true;
        s_manager.next_poll_ms = now_ms() + CONFIG_GATEWAY_MODEM_RECOVERY_BACKOFF_MS;
        return;
    }

    ESP_LOGE(TAG, "recovery: hard-resetting modem VBUS");
    esp_err_t hard_reset_err = ESP_ERR_NOT_SUPPORTED;
    if (s_manager.transport.hard_reset != NULL) {
        hard_reset_err = s_manager.transport.hard_reset(s_manager.transport.ctx);
    }
    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.last_error = hard_reset_err;
    if (hard_reset_err == ESP_OK) {
        ++s_manager.snapshot.hard_resets;
        s_manager.snapshot.initialized = false;
        s_manager.snapshot.transport_ready = false;
        s_manager.snapshot.consecutive_failures = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    s_manager.successful_poll_cycles = 0;
    s_manager.reinitialize_pending = false;
    snapshot_set_state(hard_reset_err == ESP_OK ? MODEM_MANAGER_WAIT_TRANSPORT : MODEM_MANAGER_DEGRADED);
}


static void handle_manual_restart(void)
{
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    if (!snapshot.transport_ready) {
        ESP_LOGW(TAG, "manual modem restart ignored: transport unavailable");
        return;
    }
    snapshot_set_state(MODEM_MANAGER_RECOVERY);
    portENTER_CRITICAL(&s_lock);
    ++s_manager.snapshot.recovery_attempts;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (s_manager.transport.hard_reset != NULL) {
        err = s_manager.transport.hard_reset(s_manager.transport.ctx);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++s_manager.snapshot.hard_resets;
        s_manager.snapshot.last_recovery_action = MODEM_RECOVERY_HARD_RESET;
        s_manager.snapshot.transport_ready = false;
        s_manager.snapshot.initialized = false;
        portEXIT_CRITICAL(&s_lock);
        snapshot_set_state(MODEM_MANAGER_WAIT_TRANSPORT);
        return;
    }

    ESP_LOGW(TAG, "VBUS restart unavailable; using modem functional reset");
    const bool reset_ok = execute_optional("AT+CFUN=1,1", NULL, 0, 5000, NULL);
    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.last_recovery_action = MODEM_RECOVERY_FUNCTIONAL_RESET;
    s_manager.snapshot.initialized = false;
    s_manager.snapshot.consecutive_failures = 0;
    portEXIT_CRITICAL(&s_lock);
    s_manager.reinitialize_pending = true;
    s_manager.next_poll_ms = now_ms() + CONFIG_GATEWAY_MODEM_RECOVERY_BACKOFF_MS;
    if (!reset_ok) {
        snapshot_set_state(MODEM_MANAGER_DEGRADED);
    }
}

static void handle_sim_pin(const char *pin)
{
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    if (!snapshot.transport_ready || snapshot.sim != MODEM_SIM_PIN_REQUIRED) {
        ESP_LOGW(TAG, "SIM PIN ignored because modem is not waiting for a PIN");
        return;
    }

    char command[32];
    const int written = snprintf(command, sizeof(command), "AT+CPIN=\"%s\"", pin);
    if (written <= 0 || (size_t)written >= sizeof(command)) {
        return;
    }
    const bool ok = execute_optional(command, NULL, 0, 10000, NULL);
    memset(command, 0, sizeof(command));
    if (ok) {
        s_manager.reinitialize_pending = true;
        s_manager.next_poll_ms = now_ms() + 1000;
    }
}

static void handle_transport_ready(void)
{
    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.transport_ready = true;
    s_manager.snapshot.initialized = false;
    s_manager.snapshot.last_error = ESP_OK;
    s_manager.snapshot.consecutive_failures = 0;
    portEXIT_CRITICAL(&s_lock);
    s_manager.reinitialize_pending = true;
    s_manager.next_poll_ms = now_ms();
    snapshot_set_state(MODEM_MANAGER_INITIALIZING);
}

static void handle_transport_lost(void)
{
    portENTER_CRITICAL(&s_lock);
    s_manager.snapshot.transport_ready = false;
    s_manager.snapshot.initialized = false;
    s_manager.snapshot.registered = false;
    s_manager.snapshot.roaming = false;
    s_manager.snapshot.sim = MODEM_SIM_UNKNOWN;
    s_manager.snapshot.circuit_registration.status = MODEM_REG_UNKNOWN;
    s_manager.snapshot.packet_registration.status = MODEM_REG_UNKNOWN;
    s_manager.snapshot.eps_registration.status = MODEM_REG_UNKNOWN;
    portEXIT_CRITICAL(&s_lock);
    s_manager.reinitialize_pending = false;
    snapshot_set_state(MODEM_MANAGER_WAIT_TRANSPORT);
    emit_snapshot();
}

static TickType_t next_wait_ticks(void)
{
    modem_manager_snapshot_t snapshot;
    snapshot_copy(&snapshot);
    if (!snapshot.transport_ready) {
        return portMAX_DELAY;
    }
    const int64_t remaining = s_manager.next_poll_ms - now_ms();
    if (remaining <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS((uint32_t)remaining);
}

static void manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        manager_event_t event;
        if (xQueueReceive(s_manager.queue, &event, next_wait_ticks()) == pdTRUE) {
            switch (event.id) {
            case MANAGER_EVENT_TRANSPORT_READY:
                handle_transport_ready();
                break;
            case MANAGER_EVENT_TRANSPORT_LOST:
                handle_transport_lost();
                break;
            case MANAGER_EVENT_URC:
                handle_urc_line(event.data.urc);
                break;
            case MANAGER_EVENT_SIM_PIN:
                handle_sim_pin(event.data.pin);
                memset(event.data.pin, 0, sizeof(event.data.pin));
                break;
            case MANAGER_EVENT_MANUAL_RESTART:
                handle_manual_restart();
                break;
            default:
                break;
            }
        }

        modem_manager_snapshot_t snapshot;
        snapshot_copy(&snapshot);
        if (!snapshot.transport_ready) {
            continue;
        }

        if (s_manager.reinitialize_pending && now_ms() >= s_manager.next_poll_ms) {
            s_manager.reinitialize_pending = false;
            if (!initialize_session()) {
                schedule_recovery();
                s_manager.next_poll_ms = now_ms() + CONFIG_GATEWAY_MODEM_RECOVERY_BACKOFF_MS;
            }
            continue;
        }

        if (now_ms() >= s_manager.next_poll_ms) {
            const bool ok = poll_status();
            snapshot_copy(&snapshot);
            if (!ok) {
                s_manager.successful_poll_cycles = 0;
                schedule_recovery();
            } else if (++s_manager.successful_poll_cycles >= 3) {
                s_manager.recoveries_since_hard_reset = 0;
                s_manager.successful_poll_cycles = 3;
            }
            s_manager.next_poll_ms = now_ms() +
                                     (snapshot.registered ? CONFIG_GATEWAY_MODEM_POLL_READY_MS
                                                          : CONFIG_GATEWAY_MODEM_POLL_WAIT_MS);
        }
    }
}

esp_err_t modem_manager_init(const modem_manager_transport_t *transport,
                             modem_manager_event_callback_t event_cb,
                             void *user_ctx)
{
    if (transport == NULL || transport->execute == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_manager.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.transport = *transport;
    s_manager.event_cb = event_cb;
    s_manager.event_user_ctx = user_ctx;
    s_manager.snapshot.state = MODEM_MANAGER_WAIT_TRANSPORT;
    s_manager.snapshot.sim = MODEM_SIM_UNKNOWN;
    s_manager.snapshot.signal = (modem_signal_t){.rssi = 99, .rssi_dbm = INT16_MIN, .ber = 99};
    s_manager.snapshot.circuit_registration = (modem_registration_t){.status = MODEM_REG_UNKNOWN, .access_technology = -1};
    s_manager.snapshot.packet_registration = (modem_registration_t){.status = MODEM_REG_UNKNOWN, .access_technology = -1};
    s_manager.snapshot.eps_registration = (modem_registration_t){.status = MODEM_REG_UNKNOWN, .access_technology = -1};
    s_manager.snapshot.operator_info = (modem_operator_t){.mode = -1, .format = -1, .access_technology = -1};
    s_manager.snapshot.last_recovery_action = MODEM_RECOVERY_NONE;
    s_manager.snapshot.last_at_result = AT_RESULT_TRANSPORT_UNAVAILABLE;
    s_manager.snapshot.last_modem_error_code = -1;

    s_manager.queue = xQueueCreate(MODEM_MANAGER_QUEUE_DEPTH, sizeof(manager_event_t));
    if (s_manager.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created = xTaskCreate(manager_task, "modem_mgr", MODEM_MANAGER_TASK_STACK,
                                           NULL, MODEM_MANAGER_TASK_PRIORITY, &s_manager.task);
    if (created != pdPASS) {
        vQueueDelete(s_manager.queue);
        s_manager.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_manager.initialized = true;
    ESP_LOGI(TAG, "modem manager initialized");
    return ESP_OK;
}

static void queue_simple_event(manager_event_id_t id)
{
    if (!s_manager.initialized || s_manager.queue == NULL) {
        return;
    }
    const manager_event_t event = {.id = id};
    const BaseType_t queued = id == MANAGER_EVENT_TRANSPORT_LOST
                                  ? xQueueSendToFront(s_manager.queue, &event, 0)
                                  : xQueueSend(s_manager.queue, &event, 0);
    if (queued != pdTRUE && id == MANAGER_EVENT_TRANSPORT_LOST) {
        /* A lost transport invalidates queued URCs/PIN work; prioritize recovery. */
        xQueueReset(s_manager.queue);
        (void)xQueueSendToFront(s_manager.queue, &event, 0);
    } else if (queued != pdTRUE) {
        ESP_LOGW(TAG, "manager queue full for event=%d", id);
    }
}

void modem_manager_notify_transport_ready(void)
{
    queue_simple_event(MANAGER_EVENT_TRANSPORT_READY);
}

void modem_manager_notify_transport_lost(void)
{
    queue_simple_event(MANAGER_EVENT_TRANSPORT_LOST);
}

void modem_manager_handle_urc(const char *line, void *user_ctx)
{
    (void)user_ctx;
    if (!s_manager.initialized || s_manager.queue == NULL || line == NULL) {
        return;
    }

    manager_event_t event = {.id = MANAGER_EVENT_URC};
    const size_t length = strlen(line);
    if (length >= sizeof(event.data.urc)) {
        portENTER_CRITICAL(&s_lock);
        ++s_manager.snapshot.urcs_dropped;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    memcpy(event.data.urc, line, length + 1);
    if (xQueueSend(s_manager.queue, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        ++s_manager.snapshot.urcs_dropped;
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t modem_manager_get_snapshot(modem_manager_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_manager.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    snapshot_copy(out);
    return ESP_OK;
}

esp_err_t modem_manager_request_restart(void)
{
    if (!s_manager.initialized || s_manager.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const manager_event_t event = {.id = MANAGER_EVENT_MANUAL_RESTART};
    return xQueueSend(s_manager.queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t modem_manager_submit_sim_pin(const char *pin)
{
    if (!s_manager.initialized || s_manager.queue == NULL || pin == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t length = 0;
    while (pin[length] != '\0' && length <= MODEM_MANAGER_PIN_MAX) {
        if (pin[length] < '0' || pin[length] > '9') {
            return ESP_ERR_INVALID_ARG;
        }
        ++length;
    }
    if (length < 4 || length > MODEM_MANAGER_PIN_MAX || pin[length] != '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    manager_event_t event = {.id = MANAGER_EVENT_SIM_PIN};
    memcpy(event.data.pin, pin, length + 1);
    const BaseType_t queued = xQueueSend(s_manager.queue, &event, 0);
    memset(event.data.pin, 0, sizeof(event.data.pin));
    return queued == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *modem_manager_state_name(modem_manager_state_t state)
{
    switch (state) {
    case MODEM_MANAGER_STOPPED: return "stopped";
    case MODEM_MANAGER_WAIT_TRANSPORT: return "wait_transport";
    case MODEM_MANAGER_INITIALIZING: return "initializing";
    case MODEM_MANAGER_SIM_WAIT: return "sim_wait";
    case MODEM_MANAGER_NETWORK_WAIT: return "network_wait";
    case MODEM_MANAGER_READY: return "ready";
    case MODEM_MANAGER_DEGRADED: return "degraded";
    case MODEM_MANAGER_RECOVERY: return "recovery";
    default: return "unknown";
    }
}
