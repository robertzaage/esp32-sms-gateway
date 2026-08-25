#include <assert.h>
#include <stdio.h>
#include "ota_version_policy.h"

int main(void)
{
    int cmp = 0;
    assert(ota_semver_compare("1.2.3", "1.2.2", &cmp) && cmp > 0);
    assert(ota_semver_compare("1.2.3-rc.2", "1.2.3-rc.1", &cmp) && cmp > 0);
    assert(ota_semver_compare("1.2.3", "1.2.3-rc.9", &cmp) && cmp > 0);
    assert(ota_semver_compare("0.7.0-dev", "0.6.1", &cmp) && cmp > 0);
    assert(!ota_semver_compare("1.2", "1.2.0", &cmp));
    assert(!ota_semver_compare("01.2.3", "1.2.3", &cmp));
    assert(!ota_semver_compare("1.2.3-01", "1.2.3-1", &cmp));

    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0-dev", 0,
                                     "esp32_sms_gateway", "0.7.0", 0, false, false) == OTA_VERSION_ACCEPT);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 0,
                                     "wrong", "0.8.0", 0, false, false) == OTA_VERSION_REJECT_PROJECT);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 0,
                                     "esp32_sms_gateway", "0.7.0", 0, false, false) == OTA_VERSION_REJECT_REINSTALL);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 0,
                                     "esp32_sms_gateway", "0.7.0", 0, true, false) == OTA_VERSION_ACCEPT);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 0,
                                     "esp32_sms_gateway", "0.6.9", 0, false, false) == OTA_VERSION_REJECT_DOWNGRADE);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 0,
                                     "esp32_sms_gateway", "0.6.9", 0, false, true) == OTA_VERSION_ACCEPT);
    assert(ota_version_policy_decide("esp32_sms_gateway", "0.7.0", 4,
                                     "esp32_sms_gateway", "0.8.0", 3, false, true) == OTA_VERSION_REJECT_SECURE_VERSION);
    puts("OTA version policy tests passed");
    return 0;
}
