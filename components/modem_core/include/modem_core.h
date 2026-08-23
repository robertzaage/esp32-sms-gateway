#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODEM_STATE_BOOT = 0,
    MODEM_STATE_WAIT_USB,
    MODEM_STATE_ENUMERATE,
    MODEM_STATE_FIND_AT_INTERFACE,
    MODEM_STATE_AT_PROBE,
    MODEM_STATE_SIM_CHECK,
    MODEM_STATE_NETWORK_REGISTER,
    MODEM_STATE_READY,
    MODEM_STATE_DEGRADED,
    MODEM_STATE_RECOVERY,
} modem_state_t;

esp_err_t modem_core_init(void);
modem_state_t modem_core_state(void);
const char *modem_core_state_name(modem_state_t state);

#ifdef __cplusplus
}
#endif
