#include "modem_recovery_policy.h"

modem_recovery_action_t modem_recovery_policy_next(uint32_t consecutive_failures,
                                                    uint32_t failure_threshold,
                                                    uint32_t hard_reset_after_rounds,
                                                    uint32_t *rounds_since_hard_reset)
{
    if (rounds_since_hard_reset == 0 || failure_threshold == 0 || hard_reset_after_rounds == 0 ||
        consecutive_failures < failure_threshold) {
        return MODEM_RECOVERY_NONE;
    }

    ++(*rounds_since_hard_reset);
    if (*rounds_since_hard_reset == 1) {
        return MODEM_RECOVERY_REINITIALIZE;
    }
    if (*rounds_since_hard_reset <= hard_reset_after_rounds) {
        return MODEM_RECOVERY_FUNCTIONAL_RESET;
    }

    *rounds_since_hard_reset = 0;
    return MODEM_RECOVERY_HARD_RESET;
}
