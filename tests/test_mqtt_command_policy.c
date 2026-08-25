#include <assert.h>
#include "mqtt_command_policy.h"

int main(void)
{
    assert(mqtt_command_allowed(MQTT_COMMAND_SMS_SEND, false, "{}"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_SMS_SEND, true, "{}"));
    assert(mqtt_command_allowed(MQTT_COMMAND_NATIVE_NOTIFY, false, "hello"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_NATIVE_NOTIFY, true, "hello"));
    assert(mqtt_command_allowed(MQTT_COMMAND_MODEM_RESTART, false, "PRESS"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_MODEM_RESTART, false, "press"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_MODEM_RESTART, true, "PRESS"));
    assert(mqtt_command_allowed(MQTT_COMMAND_SYSTEM_REBOOT, false, "PRESS"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_SYSTEM_REBOOT, false, ""));
    assert(mqtt_command_allowed(MQTT_COMMAND_HA_STATUS, false, "online"));
    assert(!mqtt_command_allowed(MQTT_COMMAND_HA_STATUS, true, "online"));
    return 0;
}
