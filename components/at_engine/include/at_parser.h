#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_PARSER_MAX_LINE_LENGTH 512

typedef enum {
    AT_TOKEN_LINE = 0,
    AT_TOKEN_PROMPT,
    AT_TOKEN_LINE_OVERFLOW,
} at_token_type_t;

typedef struct {
    at_token_type_t type;
    const char *text;
    size_t length;
} at_token_t;

typedef void (*at_token_callback_t)(const at_token_t *token, void *user_ctx);

typedef struct {
    char line[AT_PARSER_MAX_LINE_LENGTH + 1];
    size_t line_length;
    bool discarding_overflow;
    bool skip_prompt_space;
    bool prompt_enabled;
} at_parser_t;

void at_parser_init(at_parser_t *parser);
void at_parser_reset(at_parser_t *parser);
void at_parser_set_prompt_enabled(at_parser_t *parser, bool enabled);
void at_parser_feed(at_parser_t *parser,
                    const uint8_t *data,
                    size_t data_len,
                    at_token_callback_t callback,
                    void *user_ctx);

#ifdef __cplusplus
}
#endif
