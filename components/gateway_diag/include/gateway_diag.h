#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_DIAG_CRASH_TEXT_MAX 192

typedef enum {
    GATEWAY_SAFE_MODE_OFF = 0,
    GATEWAY_SAFE_MODE_NO_MODEM,             /* repeated crashes: USB modem stack not started */
    GATEWAY_SAFE_MODE_NO_MODEM_NO_DISPLAY,  /* still crashing: display not started either */
} gateway_safe_mode_t;

/**
 * Evaluate the previous reset. Call right after nvs_flash_init(). Counts
 * consecutive abnormal resets (panic, watchdog, brownout, USB reset), turns a
 * stored core dump into a short persistent crash summary, and decides the safe
 * mode for this boot.
 */
esp_err_t gateway_diag_init(void);

gateway_safe_mode_t gateway_diag_safe_mode(void);
/** Human-readable reason for the reset that started this boot. */
const char *gateway_diag_reset_reason(void);
/** Consecutive abnormal resets before this boot (0 after a clean start). */
uint32_t gateway_diag_crash_count(void);
/**
 * Short summary of the most recent crash ("" if none), e.g.
 * "assert failed: ... | task usb_host_lib PC 42037625 BT 4037cf25 42020c7a ...".
 * Decode the hex addresses with xtensa-esp32s3-elf-addr2line and the release ELF.
 * fresh is true when that crash ended the previous boot.
 */
const char *gateway_diag_last_crash(bool *fresh);
/** Forget the stored crash summary. */
void gateway_diag_clear_crash(void);

#ifdef __cplusplus
}
#endif
