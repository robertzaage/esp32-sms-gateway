#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODEM_RECOVERY_NONE = 0,
    MODEM_RECOVERY_REINITIALIZE,
    MODEM_RECOVERY_FUNCTIONAL_RESET,
    MODEM_RECOVERY_HARD_RESET,
} modem_recovery_action_t;

/**
 * Return the next recovery action once the failure threshold is reached.
 * `rounds_since_hard_reset` is updated by the function and reset after a hard reset.
 * With hard_reset_after_rounds=2 the sequence is reinitialize, functional reset, hard reset.
 */
modem_recovery_action_t modem_recovery_policy_next(uint32_t consecutive_failures,
                                                    uint32_t failure_threshold,
                                                    uint32_t hard_reset_after_rounds,
                                                    uint32_t *rounds_since_hard_reset);

#ifdef __cplusplus
}
#endif
