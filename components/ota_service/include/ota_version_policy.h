#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_VERSION_ACCEPT = 0,
    OTA_VERSION_REJECT_PROJECT,
    OTA_VERSION_REJECT_FORMAT,
    OTA_VERSION_REJECT_REINSTALL,
    OTA_VERSION_REJECT_DOWNGRADE,
    OTA_VERSION_REJECT_SECURE_VERSION,
} ota_version_decision_t;

/* Compare semantic versions. Returns false if either input is not SemVer-like. */
bool ota_semver_compare(const char *a, const char *b, int *comparison);

ota_version_decision_t ota_version_policy_decide(const char *expected_project,
                                                  const char *running_version,
                                                  uint32_t running_secure_version,
                                                  const char *candidate_project,
                                                  const char *candidate_version,
                                                  uint32_t candidate_secure_version,
                                                  bool allow_reinstall,
                                                  bool allow_downgrade);

#ifdef __cplusplus
}
#endif
