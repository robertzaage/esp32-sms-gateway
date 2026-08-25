#include "modem_recovery_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t rounds = 0;
    assert(modem_recovery_policy_next(2, 3, 2, &rounds) == MODEM_RECOVERY_NONE);
    assert(rounds == 0);

    assert(modem_recovery_policy_next(3, 3, 2, &rounds) == MODEM_RECOVERY_REINITIALIZE);
    assert(rounds == 1);
    assert(modem_recovery_policy_next(3, 3, 2, &rounds) == MODEM_RECOVERY_FUNCTIONAL_RESET);
    assert(rounds == 2);
    assert(modem_recovery_policy_next(3, 3, 2, &rounds) == MODEM_RECOVERY_HARD_RESET);
    assert(rounds == 0);

    assert(modem_recovery_policy_next(3, 0, 2, &rounds) == MODEM_RECOVERY_NONE);
    assert(modem_recovery_policy_next(3, 3, 0, &rounds) == MODEM_RECOVERY_NONE);
    assert(modem_recovery_policy_next(3, 3, 2, NULL) == MODEM_RECOVERY_NONE);

    puts("modem recovery policy tests passed");
    return 0;
}
