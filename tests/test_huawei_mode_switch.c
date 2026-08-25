#include <assert.h>
#include <string.h>
#include "huawei_mode_switch.h"

int main(void)
{
    assert(huawei_mode_switch_pid_supported(0x1446, 0));
    assert(huawei_mode_switch_pid_supported(0x14fe, 0));
    assert(huawei_mode_switch_pid_supported(0x1f01, 0x1f01));
    assert(!huawei_mode_switch_pid_supported(0x1506, 0));
    uint8_t msg[HUAWEI_MODE_SWITCH_MESSAGE_LEN];
    assert(huawei_mode_switch_message(msg, sizeof(msg)));
    const uint8_t prefix[] = {0x55,0x53,0x42,0x43,0x12,0x34,0x56,0x78};
    assert(memcmp(msg, prefix, sizeof(prefix)) == 0);
    assert(msg[15] == 0x11 && msg[16] == 0x06 && msg[17] == 0x20);
    return 0;
}
