#include "at_protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const DEFAULT_URC_PREFIXES[] = {
    "+CMTI:",
    "+CMT:",
    "+CDS:",
    "+CDSI:",
    "+CBM:",
    "+CREG:",
    "+CGREG:",
    "+CEREG:",
    "+CUSD:",
    "+CLIP:",
    "+CRING:",
    "+CGEV:",
    "+CPIN:",
    "+CFUN:",
    "^RSSI:",
    "^MODE:",
    "^SRVST:",
    "^BOOT",
    "^DSFLOWRPT:",
    "^HCSQ:",
    "RING",
    "NO CARRIER",
    "SMS READY",
    "PB DONE",
};

bool at_protocol_has_prefix(const char *line, const char *prefix)
{
    if (line == NULL || prefix == NULL) {
        return false;
    }
    const size_t prefix_len = strlen(prefix);
    return strncmp(line, prefix, prefix_len) == 0;
}

bool at_protocol_matches_any_prefix(const char *line,
                                    const char *const *prefixes,
                                    size_t prefix_count)
{
    if (line == NULL || prefixes == NULL) {
        return false;
    }
    for (size_t i = 0; i < prefix_count; ++i) {
        if (prefixes[i] != NULL && at_protocol_has_prefix(line, prefixes[i])) {
            return true;
        }
    }
    return false;
}

bool at_protocol_is_default_urc(const char *line)
{
    return at_protocol_matches_any_prefix(
        line, DEFAULT_URC_PREFIXES, sizeof(DEFAULT_URC_PREFIXES) / sizeof(DEFAULT_URC_PREFIXES[0]));
}

size_t at_protocol_urc_continuation_lines(const char *line)
{
    if (line == NULL) {
        return 0;
    }
    /* +CMT, +CDS and +CBM are followed by one payload/PDU line. */
    return at_protocol_has_prefix(line, "+CMT:") ||
                   at_protocol_has_prefix(line, "+CDS:") ||
                   at_protocol_has_prefix(line, "+CBM:")
               ? 1U
               : 0U;
}

bool at_protocol_should_route_as_urc(const char *line,
                                     const char *const *expected_prefixes,
                                     size_t expected_prefix_count)
{
    if (!at_protocol_is_default_urc(line)) {
        return false;
    }
    return !at_protocol_matches_any_prefix(line, expected_prefixes, expected_prefix_count);
}

static at_final_status_t parse_prefixed_error(const char *line,
                                              const char *prefix,
                                              at_final_kind_t kind)
{
    at_final_status_t status = {.kind = kind, .error_code = -1};
    const char *value = line + strlen(prefix);
    while (*value == ' ' || *value == '\t') {
        ++value;
    }

    if (*value == '\0') {
        return status;
    }

    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end != value) {
        while (*end == ' ' || *end == '\t') {
            ++end;
        }
        if (*end == '\0' && parsed >= 0 && parsed <= 2147483647L) {
            status.error_code = (int)parsed;
        }
    }
    return status;
}

at_final_status_t at_protocol_final_status(const char *line)
{
    if (line == NULL) {
        return (at_final_status_t){.kind = AT_FINAL_NONE, .error_code = -1};
    }
    if (strcmp(line, "OK") == 0) {
        return (at_final_status_t){.kind = AT_FINAL_OK, .error_code = -1};
    }
    if (strcmp(line, "ERROR") == 0) {
        return (at_final_status_t){.kind = AT_FINAL_ERROR, .error_code = -1};
    }
    if (at_protocol_has_prefix(line, "+CME ERROR:")) {
        return parse_prefixed_error(line, "+CME ERROR:", AT_FINAL_CME_ERROR);
    }
    if (at_protocol_has_prefix(line, "+CMS ERROR:")) {
        return parse_prefixed_error(line, "+CMS ERROR:", AT_FINAL_CMS_ERROR);
    }
    return (at_final_status_t){.kind = AT_FINAL_NONE, .error_code = -1};
}
