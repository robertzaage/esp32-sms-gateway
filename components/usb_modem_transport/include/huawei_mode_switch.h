#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUAWEI_USB_VID 0x12D1U
#define HUAWEI_MODE_SWITCH_MESSAGE_LEN 31U

bool huawei_mode_switch_pid_supported(uint16_t pid, uint16_t extra_pid);
bool huawei_mode_switch_message(uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif
