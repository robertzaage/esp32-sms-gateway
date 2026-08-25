#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AT_FINAL_NONE = 0,
    AT_FINAL_OK,
    AT_FINAL_ERROR,
    AT_FINAL_CME_ERROR,
    AT_FINAL_CMS_ERROR,
} at_final_kind_t;

typedef struct {
    at_final_kind_t kind;
    int error_code; /* -1 when the modem returned text or no numeric code. */
} at_final_status_t;

at_final_status_t at_protocol_final_status(const char *line);
bool at_protocol_has_prefix(const char *line, const char *prefix);
bool at_protocol_is_default_urc(const char *line);
size_t at_protocol_urc_continuation_lines(const char *line);
bool at_protocol_matches_any_prefix(const char *line,
                                    const char *const *prefixes,
                                    size_t prefix_count);

/**
 * A known URC is a response line when the active command explicitly expects
 * that prefix. This is necessary for commands such as AT+CREG? whose response
 * prefix is also used by unsolicited registration notifications.
 */
bool at_protocol_should_route_as_urc(const char *line,
                                     const char *const *expected_prefixes,
                                     size_t expected_prefix_count);

#ifdef __cplusplus
}
#endif
