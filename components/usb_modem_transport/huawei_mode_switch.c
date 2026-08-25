#include "huawei_mode_switch.h"

#include <string.h>

bool huawei_mode_switch_pid_supported(uint16_t pid, uint16_t extra_pid)
{
    return pid == 0x1446U || pid == 0x14FEU || (extra_pid != 0U && pid == extra_pid);
}

bool huawei_mode_switch_message(uint8_t *out, size_t capacity)
{
    static const uint8_t message[HUAWEI_MODE_SWITCH_MESSAGE_LEN] = {
        0x55, 0x53, 0x42, 0x43, 0x12, 0x34, 0x56, 0x78,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
        0x06, 0x20, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    if (out == NULL || capacity < sizeof(message)) return false;
    memcpy(out, message, sizeof(message));
    return true;
}
