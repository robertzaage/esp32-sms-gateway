#include "sdkconfig.h"
#include <inttypes.h>
#include "modem_core.h"

#include "gateway_board.h"
#include "sms_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "modem_core";
static modem_state_t s_state = MODEM_STATE_BOOT;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static modem_manager_state_t s_last_manager_state = MODEM_MANAGER_STOPPED;
static sms_service_event_callback_t s_external_sms_event_cb;
static void *s_external_sms_event_ctx;

static esp_err_t at_transport_write(void *ctx, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)ctx;
    return modem_usb_transport_write(data, len, timeout_ms);
}

static bool at_transport_ready(void *ctx)
{
    (void)ctx;
    return modem_usb_transport_is_ready();
}

static void usb_rx_to_at_engine(const uint8_t *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    at_engine_feed_rx(data, len);
}

static esp_err_t manager_execute(void *ctx, const at_request_t *request, at_response_t *response)
{
    (void)ctx;
    return at_engine_execute(request, response);
}

static bool sms_modem_ready(void *ctx)
{
    (void)ctx;
    modem_manager_snapshot_t snapshot;
    return modem_usb_transport_is_ready() &&
           modem_manager_get_snapshot(&snapshot) == ESP_OK &&
           snapshot.state == MODEM_MANAGER_READY;
}

static void core_urc_dispatch(const char *line, void *user_ctx)
{
    (void)user_ctx;
    modem_manager_handle_urc(line, NULL);
    sms_service_handle_urc(line, NULL);
}

static void sms_event(sms_service_event_t event, const sms_message_t *message, void *user_ctx)
{
    (void)user_ctx;
    if (message == NULL) {
        return;
    }
    ESP_LOGI(TAG, "sms event=%d id=%" PRIu32 " direction=%d status=%s segments=%u",
             event, message->id, message->direction, sms_message_status_name(message->status),
             (unsigned)message->segment_count);
    if (s_external_sms_event_cb != NULL) {
        s_external_sms_event_cb(event, message, s_external_sms_event_ctx);
    }
}

static esp_err_t manager_hard_reset(void *ctx)
{
    (void)ctx;
    if (!gateway_board_usb_host_power_control_available()) {
        ESP_LOGE(TAG, "cannot power-cycle modem: Type-A VBUS source is %s",
                 gateway_board_usb_host_power_source_name());
        return ESP_ERR_NOT_SUPPORTED;
    }
    at_engine_notify_transport_lost();
    ESP_LOGW(TAG, "power-cycling USB host VBUS source=%s for modem recovery",
             gateway_board_usb_host_power_source_name());
    esp_err_t err = gateway_board_usb_host_power_set(false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_GATEWAY_MODEM_HARD_RESET_OFF_MS));
    return gateway_board_usb_host_power_set(true);
}

static void set_state(modem_state_t state, const modem_usb_diagnostics_t *diagnostics)
{
    portENTER_CRITICAL(&s_state_lock);
    const modem_state_t previous = s_state;
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);

    if (previous != state) {
        if (diagnostics != NULL) {
            ESP_LOGI(TAG, "state=%s -> %s usb=%04x:%04x if=%u at=%d",
                     modem_core_state_name(previous), modem_core_state_name(state),
                     (unsigned)diagnostics->vid, (unsigned)diagnostics->pid,
                     (unsigned)diagnostics->interface_number, diagnostics->at_ready);
        } else {
            ESP_LOGI(TAG, "state=%s -> %s", modem_core_state_name(previous), modem_core_state_name(state));
        }
    }
}

static void manager_event(const modem_manager_snapshot_t *snapshot, void *user_ctx)
{
    (void)user_ctx;
    if (snapshot == NULL) {
        return;
    }

    const modem_manager_state_t previous_manager_state = s_last_manager_state;
    s_last_manager_state = snapshot->state;
    if (snapshot->state == MODEM_MANAGER_READY && previous_manager_state != MODEM_MANAGER_READY) {
        sms_service_notify_modem_ready();
    } else if (previous_manager_state == MODEM_MANAGER_READY && snapshot->state != MODEM_MANAGER_READY) {
        sms_service_notify_modem_lost();
    }

    switch (snapshot->state) {
    case MODEM_MANAGER_WAIT_TRANSPORT:
        set_state(MODEM_STATE_WAIT_USB, NULL);
        break;
    case MODEM_MANAGER_INITIALIZING:
        set_state(MODEM_STATE_SIM_CHECK, NULL);
        break;
    case MODEM_MANAGER_SIM_WAIT:
        set_state(MODEM_STATE_SIM_CHECK, NULL);
        break;
    case MODEM_MANAGER_NETWORK_WAIT:
        set_state(MODEM_STATE_NETWORK_REGISTER, NULL);
        break;
    case MODEM_MANAGER_READY:
        set_state(MODEM_STATE_READY, NULL);
        break;
    case MODEM_MANAGER_RECOVERY:
        set_state(MODEM_STATE_RECOVERY, NULL);
        break;
    case MODEM_MANAGER_DEGRADED:
        set_state(MODEM_STATE_DEGRADED, NULL);
        break;
    case MODEM_MANAGER_STOPPED:
    default:
        break;
    }
}

static void usb_transport_event(modem_usb_event_t event,
                                const modem_usb_diagnostics_t *diagnostics,
                                void *user_ctx)
{
    (void)user_ctx;

    switch (event) {
    case MODEM_USB_EVENT_HOST_READY:
        set_state(MODEM_STATE_WAIT_USB, diagnostics);
        break;
    case MODEM_USB_EVENT_DEVICE_FOUND:
        set_state(MODEM_STATE_ENUMERATE, diagnostics);
        set_state(MODEM_STATE_FIND_AT_INTERFACE, diagnostics);
        break;
    case MODEM_USB_EVENT_PROBING:
        set_state(MODEM_STATE_AT_PROBE, diagnostics);
        break;
    case MODEM_USB_EVENT_AT_READY:
        set_state(MODEM_STATE_AT_READY, diagnostics);
        modem_manager_notify_transport_ready();
        break;
    case MODEM_USB_EVENT_DISCONNECTED:
        at_engine_notify_transport_lost();
        sms_service_notify_modem_lost();
        modem_manager_notify_transport_lost();
        set_state(MODEM_STATE_WAIT_USB, diagnostics);
        break;
    case MODEM_USB_EVENT_ERROR:
        at_engine_notify_transport_lost();
        sms_service_notify_modem_lost();
        modem_manager_notify_transport_lost();
        set_state(MODEM_STATE_DEGRADED, diagnostics);
        break;
    default:
        ESP_LOGW(TAG, "unknown USB transport event=%d", event);
        break;
    }
}

esp_err_t modem_core_init(void)
{
    set_state(MODEM_STATE_WAIT_USB, NULL);

    const at_transport_t at_transport = {
        .write = at_transport_write,
        .is_ready = at_transport_ready,
        .ctx = NULL,
        .write_timeout_ms = 1000,
    };
    esp_err_t err = at_engine_init(&at_transport);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize AT engine: %s", esp_err_to_name(err));
        set_state(MODEM_STATE_DEGRADED, NULL);
        return err;
    }

    const modem_manager_transport_t manager_transport = {
        .execute = manager_execute,
        .hard_reset = manager_hard_reset,
        .ctx = NULL,
    };
    err = modem_manager_init(&manager_transport, manager_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize modem manager: %s", esp_err_to_name(err));
        set_state(MODEM_STATE_DEGRADED, NULL);
        return err;
    }

    const sms_service_transport_t sms_transport = {
        .execute = manager_execute,
        .is_ready = sms_modem_ready,
        .ctx = NULL,
    };
    err = sms_service_init(&sms_transport, sms_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize SMS service: %s", esp_err_to_name(err));
        set_state(MODEM_STATE_DEGRADED, NULL);
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(at_engine_set_urc_callback(core_urc_dispatch, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(modem_usb_transport_set_rx_callback(usb_rx_to_at_engine, NULL));

    err = modem_usb_transport_start(usb_transport_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start USB modem transport: %s", esp_err_to_name(err));
        set_state(MODEM_STATE_DEGRADED, NULL);
    }
    return err;
}

modem_state_t modem_core_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const modem_state_t state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

const char *modem_core_state_name(modem_state_t state)
{
    switch (state) {
    case MODEM_STATE_BOOT: return "boot";
    case MODEM_STATE_WAIT_USB: return "wait_usb";
    case MODEM_STATE_ENUMERATE: return "enumerate";
    case MODEM_STATE_FIND_AT_INTERFACE: return "find_at_interface";
    case MODEM_STATE_AT_PROBE: return "at_probe";
    case MODEM_STATE_AT_READY: return "at_ready";
    case MODEM_STATE_SIM_CHECK: return "sim_check";
    case MODEM_STATE_NETWORK_REGISTER: return "network_register";
    case MODEM_STATE_READY: return "ready";
    case MODEM_STATE_DEGRADED: return "degraded";
    case MODEM_STATE_RECOVERY: return "recovery";
    default: return "unknown";
    }
}

esp_err_t modem_core_usb_diagnostics(modem_usb_diagnostics_t *out)
{
    return modem_usb_transport_get_diagnostics(out);
}

esp_err_t modem_core_at_diagnostics(at_engine_diagnostics_t *out)
{
    return at_engine_get_diagnostics(out);
}

esp_err_t modem_core_manager_snapshot(modem_manager_snapshot_t *out)
{
    return modem_manager_get_snapshot(out);
}

esp_err_t modem_core_submit_sim_pin(const char *pin)
{
    return modem_manager_submit_sim_pin(pin);
}

esp_err_t modem_core_restart_modem(void)
{
    return modem_manager_request_restart();
}

esp_err_t modem_core_at_execute(const at_request_t *request, at_response_t *response)
{
    return at_engine_execute(request, response);
}


esp_err_t modem_core_sms_send(const char *recipient, const char *text, bool delivery_report, uint32_t *message_id)
{
    return sms_service_send(recipient, text, delivery_report, message_id);
}

esp_err_t modem_core_sms_get(uint32_t id, sms_message_t *out)
{
    return sms_service_get(id, out);
}

esp_err_t modem_core_sms_list(uint32_t after_id, sms_message_t *records, size_t max_records, size_t *record_count)
{
    return sms_service_list(after_id, records, max_records, record_count);
}

esp_err_t modem_core_sms_delete(uint32_t id)
{
    return sms_service_delete(id);
}

esp_err_t modem_core_sms_retry_uncertain(uint32_t id)
{
    return sms_service_retry_uncertain(id);
}

esp_err_t modem_core_sms_diagnostics(sms_service_diagnostics_t *out)
{
    return sms_service_get_diagnostics(out);
}

void modem_core_sms_set_event_replay_watermark(bool protection_enabled, uint32_t watermark)
{
    sms_service_set_event_replay_watermark(protection_enabled, watermark);
}


esp_err_t modem_core_set_sms_event_callback(sms_service_event_callback_t cb, void *user_ctx)
{
    s_external_sms_event_cb = cb;
    s_external_sms_event_ctx = user_ctx;
    return ESP_OK;
}
