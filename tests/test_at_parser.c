#include "at_parser.h"
#include "at_protocol.h"
#include "at_router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char tokens[64][AT_PARSER_MAX_LINE_LENGTH + 16];
    at_token_type_t types[64];
    size_t count;
} capture_t;

static void capture_token(const at_token_t *token, void *ctx)
{
    capture_t *capture = (capture_t *)ctx;
    assert(capture->count < 64);
    capture->types[capture->count] = token->type;
    if (token->text != NULL) {
        assert(token->length + 1 < sizeof(capture->tokens[capture->count]));
        memcpy(capture->tokens[capture->count], token->text, token->length);
        capture->tokens[capture->count][token->length] = '\0';
    }
    ++capture->count;
}

static void feed_fragmented(const char *input, unsigned seed, capture_t *capture, bool prompt_enabled)
{
    at_parser_t parser;
    at_parser_init(&parser);
    at_parser_set_prompt_enabled(&parser, prompt_enabled);

    size_t offset = 0;
    const size_t length = strlen(input);
    while (offset < length) {
        seed = seed * 1103515245U + 12345U;
        size_t chunk = 1 + ((seed >> 16) % 11U);
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        at_parser_feed(&parser, (const uint8_t *)input + offset, chunk, capture_token, capture);
        offset += chunk;
    }
}

static void test_fragmentation_invariance(void)
{
    const char *stream =
        "AT+CSQ\r\r\n"
        "+CSQ: 15,99\r\n"
        "+CMTI: \"SM\",7\r\n"
        "OK\r\n";

    capture_t baseline = {0};
    feed_fragmented(stream, 1, &baseline, false);
    assert(baseline.count == 4);
    assert(strcmp(baseline.tokens[0], "AT+CSQ") == 0);
    assert(strcmp(baseline.tokens[1], "+CSQ: 15,99") == 0);
    assert(strcmp(baseline.tokens[2], "+CMTI: \"SM\",7") == 0);
    assert(strcmp(baseline.tokens[3], "OK") == 0);

    for (unsigned seed = 2; seed < 2000; ++seed) {
        capture_t actual = {0};
        feed_fragmented(stream, seed, &actual, false);
        assert(actual.count == baseline.count);
        for (size_t i = 0; i < baseline.count; ++i) {
            assert(actual.types[i] == baseline.types[i]);
            assert(strcmp(actual.tokens[i], baseline.tokens[i]) == 0);
        }
    }
}

static void test_prompt_mode(void)
{
    capture_t capture = {0};
    at_parser_t parser;
    at_parser_init(&parser);

    const char *ordinary = "> not a modem prompt\r\n";
    at_parser_feed(&parser, (const uint8_t *)ordinary, strlen(ordinary), capture_token, &capture);
    assert(capture.count == 1);
    assert(capture.types[0] == AT_TOKEN_LINE);
    assert(strcmp(capture.tokens[0], "> not a modem prompt") == 0);

    memset(&capture, 0, sizeof(capture));
    at_parser_reset(&parser);
    at_parser_set_prompt_enabled(&parser, true);
    const char *prompt = "\r\n> ";
    at_parser_feed(&parser, (const uint8_t *)prompt, strlen(prompt), capture_token, &capture);
    assert(capture.count == 1);
    assert(capture.types[0] == AT_TOKEN_PROMPT);
}

static void test_line_overflow_recovers(void)
{
    at_parser_t parser;
    at_parser_init(&parser);
    capture_t capture = {0};

    char long_line[AT_PARSER_MAX_LINE_LENGTH + 32];
    memset(long_line, 'A', sizeof(long_line));
    at_parser_feed(&parser, (const uint8_t *)long_line, sizeof(long_line), capture_token, &capture);
    const char *tail = "\r\nOK\r\n";
    at_parser_feed(&parser, (const uint8_t *)tail, strlen(tail), capture_token, &capture);

    assert(capture.count == 2);
    assert(capture.types[0] == AT_TOKEN_LINE_OVERFLOW);
    assert(capture.types[1] == AT_TOKEN_LINE);
    assert(strcmp(capture.tokens[1], "OK") == 0);
}

static void test_final_results(void)
{
    at_final_status_t status = at_protocol_final_status("OK");
    assert(status.kind == AT_FINAL_OK && status.error_code == -1);

    status = at_protocol_final_status("ERROR");
    assert(status.kind == AT_FINAL_ERROR && status.error_code == -1);

    status = at_protocol_final_status("+CME ERROR: 13");
    assert(status.kind == AT_FINAL_CME_ERROR && status.error_code == 13);

    status = at_protocol_final_status("+CMS ERROR: 500");
    assert(status.kind == AT_FINAL_CMS_ERROR && status.error_code == 500);

    status = at_protocol_final_status("+CMS ERROR: operation not allowed");
    assert(status.kind == AT_FINAL_CMS_ERROR && status.error_code == -1);
}

static void test_urc_routing_rules(void)
{
    const char *creg_expected[] = {"+CREG:"};
    assert(at_protocol_should_route_as_urc("+CREG: 1", NULL, 0));
    assert(!at_protocol_should_route_as_urc("+CREG: 0,1", creg_expected, 1));
    assert(at_protocol_should_route_as_urc("+CMTI: \"SM\",7", creg_expected, 1));
    assert(at_protocol_should_route_as_urc("+CDSI: \"SM\",8", creg_expected, 1));
    assert(!at_protocol_should_route_as_urc("+CSQ: 18,99", NULL, 0));
    assert(at_protocol_should_route_as_urc("^RSSI: 15", NULL, 0));
    const char *hcsq_expected[] = {"^HCSQ:"};
    assert(!at_protocol_should_route_as_urc("^HCSQ: WCDMA,44,35,60", hcsq_expected, 1));
    assert(at_protocol_urc_continuation_lines("+CMT: ,23") == 1);
    assert(at_protocol_urc_continuation_lines("+CDS: 23") == 1);
    assert(at_protocol_urc_continuation_lines("+CMTI: \"SM\",7") == 0);
}

static void test_interleaved_multiline_urc_routing(void)
{
    const char *expected[] = {"+CSQ:"};
    at_router_t router;
    at_router_init(&router);

    assert(at_router_route_line(&router, "+CSQ: 18,99", expected, 1) == AT_ROUTE_RESPONSE);
    assert(at_router_route_line(&router, "+CMT: ,23", expected, 1) == AT_ROUTE_URC);
    /* A URC payload that looks like a final result still belongs to the URC. */
    assert(at_router_route_line(&router, "OK", expected, 1) == AT_ROUTE_URC);
    assert(at_router_route_line(&router, "+CSQ: 19,99", expected, 1) == AT_ROUTE_RESPONSE);

    assert(at_router_route_line(&router, "+CDS: 23", expected, 1) == AT_ROUTE_URC);
    assert(at_router_route_line(&router, "0791448720003024040B914407281553F600001260", expected, 1) == AT_ROUTE_URC);

    const char *creg_expected[] = {"+CREG:"};
    assert(at_router_route_line(&router, "+CREG: 0,1", creg_expected, 1) == AT_ROUTE_RESPONSE);
    assert(at_router_route_line(&router, "+CMTI: \"SM\",9", creg_expected, 1) == AT_ROUTE_URC);
}

int main(void)
{
    test_fragmentation_invariance();
    test_prompt_mode();
    test_line_overflow_recovers();
    test_final_results();
    test_urc_routing_rules();
    test_interleaved_multiline_urc_routing();
    puts("AT parser/protocol tests passed");
    return 0;
}
