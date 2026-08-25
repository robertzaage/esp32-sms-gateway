#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTT_COMMAND_NONE = 0,
    MQTT_COMMAND_SMS_SEND,
    MQTT_COMMAND_NATIVE_NOTIFY,
    MQTT_COMMAND_MODEM_RESTART,
    MQTT_COMMAND_SYSTEM_REBOOT,
    MQTT_COMMAND_HA_STATUS,
} mqtt_command_kind_t;

/** Retained MQTT application commands are never executed. */
bool mqtt_command_allowed(mqtt_command_kind_t kind, bool retained, const char *payload);

#ifdef __cplusplus
}
#endif
