#include "at_router.h"

#include <string.h>

#include "at_protocol.h"

void at_router_init(at_router_t *router)
{
    if (router != NULL) {
        memset(router, 0, sizeof(*router));
    }
}

at_route_t at_router_route_line(at_router_t *router,
                                const char *line,
                                const char *const *expected_prefixes,
                                size_t expected_prefix_count)
{
    if (router == NULL || line == NULL) {
        return AT_ROUTE_RESPONSE;
    }

    if (router->urc_continuation_lines > 0) {
        --router->urc_continuation_lines;
        return AT_ROUTE_URC;
    }

    if (at_protocol_should_route_as_urc(line, expected_prefixes, expected_prefix_count)) {
        router->urc_continuation_lines = at_protocol_urc_continuation_lines(line);
        return AT_ROUTE_URC;
    }

    return AT_ROUTE_RESPONSE;
}
