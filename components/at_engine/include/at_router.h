#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AT_ROUTE_RESPONSE = 0,
    AT_ROUTE_URC,
} at_route_t;

typedef struct {
    size_t urc_continuation_lines;
} at_router_t;

void at_router_init(at_router_t *router);
at_route_t at_router_route_line(at_router_t *router,
                                const char *line,
                                const char *const *expected_prefixes,
                                size_t expected_prefix_count);

#ifdef __cplusplus
}
#endif
