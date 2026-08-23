#include "modem_core.h"

#include "esp_log.h"

static const char *TAG = "modem_core";
static modem_state_t s_state = MODEM_STATE_BOOT;

esp_err_t modem_core_init(void)
{
    /* USB transport is intentionally introduced as the next isolated milestone. */
    s_state = MODEM_STATE_WAIT_USB;
    ESP_LOGI(TAG, "state=%s", modem_core_state_name(s_state));
    return ESP_OK;
}

modem_state_t modem_core_state(void)
{
    return s_state;
}

const char *modem_core_state_name(modem_state_t state)
{
    switch (state) {
    case MODEM_STATE_BOOT: return "boot";
    case MODEM_STATE_WAIT_USB: return "wait_usb";
    case MODEM_STATE_ENUMERATE: return "enumerate";
    case MODEM_STATE_FIND_AT_INTERFACE: return "find_at_interface";
    case MODEM_STATE_AT_PROBE: return "at_probe";
    case MODEM_STATE_SIM_CHECK: return "sim_check";
    case MODEM_STATE_NETWORK_REGISTER: return "network_register";
    case MODEM_STATE_READY: return "ready";
    case MODEM_STATE_DEGRADED: return "degraded";
    case MODEM_STATE_RECOVERY: return "recovery";
    default: return "unknown";
    }
}
