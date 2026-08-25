#include "ota_version_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    const char *pre;
    size_t pre_len;
} semver_t;

static bool parse_number(const char **cursor, unsigned long *value)
{
    const char *p = *cursor;
    if (!isdigit((unsigned char)*p)) return false;
    if (*p == '0' && isdigit((unsigned char)p[1])) return false;
    unsigned long out = 0;
    while (isdigit((unsigned char)*p)) {
        const unsigned digit = (unsigned)(*p - '0');
        if (out > (ULONG_MAX - digit) / 10UL) return false;
        out = out * 10UL + digit;
        ++p;
    }
    *cursor = p;
    *value = out;
    return true;
}

static bool valid_identifier_char(char c)
{
    return isalnum((unsigned char)c) || c == '-';
}

static bool valid_prerelease_sequence(const char *s, size_t len)
{
    size_t start = 0;
    while (start < len) {
        size_t end = start;
        bool numeric = true;
        while (end < len && s[end] != '.') {
            if (!isdigit((unsigned char)s[end])) numeric = false;
            ++end;
        }
        if (end == start || (numeric && end - start > 1 && s[start] == '0')) return false;
        start = end < len ? end + 1 : end;
    }
    return true;
}

static bool parse_semver(const char *text, semver_t *out)
{
    if (text == NULL || out == NULL || *text == '\0') return false;
    memset(out, 0, sizeof(*out));
    const char *p = text;
    if (!parse_number(&p, &out->major) || *p++ != '.' ||
        !parse_number(&p, &out->minor) || *p++ != '.' ||
        !parse_number(&p, &out->patch)) return false;

    if (*p == '-') {
        const char *start = ++p;
        bool previous_dot = true;
        while (*p && *p != '+') {
            if (*p == '.') {
                if (previous_dot) return false;
                previous_dot = true;
            } else {
                if (!valid_identifier_char(*p)) return false;
                previous_dot = false;
            }
            ++p;
        }
        if (p == start || previous_dot) return false;
        out->pre = start;
        out->pre_len = (size_t)(p - start);
        if (!valid_prerelease_sequence(out->pre, out->pre_len)) return false;
    }

    if (*p == '+') {
        ++p;
        bool previous_dot = true;
        const char *start = p;
        while (*p) {
            if (*p == '.') {
                if (previous_dot) return false;
                previous_dot = true;
            } else {
                if (!valid_identifier_char(*p)) return false;
                previous_dot = false;
            }
            ++p;
        }
        if (p == start || previous_dot) return false;
    }
    return *p == '\0';
}

static bool identifier_numeric(const char *s, size_t len)
{
    if (len == 0) return false;
    if (len > 1 && s[0] == '0') return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static int compare_identifiers(const char *a, size_t alen, const char *b, size_t blen)
{
    const bool an = identifier_numeric(a, alen);
    const bool bn = identifier_numeric(b, blen);
    if (an && bn) {
        if (alen != blen) return alen < blen ? -1 : 1;
        const int cmp = memcmp(a, b, alen);
        return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
    }
    if (an != bn) return an ? -1 : 1;
    const size_t n = alen < blen ? alen : blen;
    const int cmp = memcmp(a, b, n);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

static int compare_prerelease(const semver_t *a, const semver_t *b)
{
    if (a->pre_len == 0 && b->pre_len == 0) return 0;
    if (a->pre_len == 0) return 1;
    if (b->pre_len == 0) return -1;

    size_t ai = 0, bi = 0;
    while (ai < a->pre_len && bi < b->pre_len) {
        size_t ae = ai, be = bi;
        while (ae < a->pre_len && a->pre[ae] != '.') ++ae;
        while (be < b->pre_len && b->pre[be] != '.') ++be;
        const int cmp = compare_identifiers(a->pre + ai, ae - ai, b->pre + bi, be - bi);
        if (cmp != 0) return cmp;
        ai = ae < a->pre_len ? ae + 1 : ae;
        bi = be < b->pre_len ? be + 1 : be;
    }
    return ai < a->pre_len ? 1 : bi < b->pre_len ? -1 : 0;
}

bool ota_semver_compare(const char *a, const char *b, int *comparison)
{
    if (comparison == NULL) return false;
    semver_t av = {0}, bv = {0};
    if (!parse_semver(a, &av) || !parse_semver(b, &bv)) return false;
    if (av.major != bv.major) { *comparison = av.major < bv.major ? -1 : 1; return true; }
    if (av.minor != bv.minor) { *comparison = av.minor < bv.minor ? -1 : 1; return true; }
    if (av.patch != bv.patch) { *comparison = av.patch < bv.patch ? -1 : 1; return true; }
    *comparison = compare_prerelease(&av, &bv);
    return true;
}

ota_version_decision_t ota_version_policy_decide(const char *expected_project,
                                                  const char *running_version,
                                                  uint32_t running_secure_version,
                                                  const char *candidate_project,
                                                  const char *candidate_version,
                                                  uint32_t candidate_secure_version,
                                                  bool allow_reinstall,
                                                  bool allow_downgrade)
{
    if (expected_project == NULL || candidate_project == NULL || strcmp(expected_project, candidate_project) != 0) {
        return OTA_VERSION_REJECT_PROJECT;
    }
    if (candidate_secure_version < running_secure_version) {
        return OTA_VERSION_REJECT_SECURE_VERSION;
    }
    int cmp = 0;
    if (!ota_semver_compare(candidate_version, running_version, &cmp)) {
        return OTA_VERSION_REJECT_FORMAT;
    }
    if (cmp == 0 && !allow_reinstall) return OTA_VERSION_REJECT_REINSTALL;
    if (cmp < 0 && !allow_downgrade) return OTA_VERSION_REJECT_DOWNGRADE;
    return OTA_VERSION_ACCEPT;
}
