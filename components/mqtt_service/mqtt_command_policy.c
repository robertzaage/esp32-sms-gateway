#include "mqtt_command_policy.h"

#include <string.h>

bool mqtt_command_allowed(mqtt_command_kind_t kind, bool retained, const char *payload)
{
    if (kind == MQTT_COMMAND_NONE) {
        return false;
    }
    if (kind == MQTT_COMMAND_HA_STATUS) {
        return !retained && payload != NULL && strcmp(payload, "online") == 0;
    }
    if (retained) {
        return false;
    }
    if (kind == MQTT_COMMAND_MODEM_RESTART || kind == MQTT_COMMAND_SYSTEM_REBOOT) {
        return payload != NULL && strcmp(payload, "PRESS") == 0;
    }
    return payload != NULL;
}
