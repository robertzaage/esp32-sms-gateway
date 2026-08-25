#include "at_parser.h"

#include <string.h>

static void emit_line(at_parser_t *parser, at_token_callback_t callback, void *user_ctx)
{
    if (parser->line_length == 0 || callback == NULL) {
        parser->line_length = 0;
        return;
    }

    parser->line[parser->line_length] = '\0';
    const at_token_t token = {
        .type = AT_TOKEN_LINE,
        .text = parser->line,
        .length = parser->line_length,
    };
    callback(&token, user_ctx);
    parser->line_length = 0;
}

void at_parser_init(at_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void at_parser_reset(at_parser_t *parser)
{
    at_parser_init(parser);
}

void at_parser_set_prompt_enabled(at_parser_t *parser, bool enabled)
{
    if (parser != NULL) {
        parser->prompt_enabled = enabled;
        if (!enabled) {
            parser->skip_prompt_space = false;
        }
    }
}

void at_parser_feed(at_parser_t *parser,
                    const uint8_t *data,
                    size_t data_len,
                    at_token_callback_t callback,
                    void *user_ctx)
{
    if (parser == NULL || data == NULL) {
        return;
    }

    for (size_t i = 0; i < data_len; ++i) {
        const char ch = (char)data[i];

        if (parser->skip_prompt_space) {
            parser->skip_prompt_space = false;
            if (ch == ' ') {
                continue;
            }
        }

        if (parser->discarding_overflow) {
            if (ch == '\r' || ch == '\n') {
                parser->discarding_overflow = false;
                parser->line_length = 0;
                if (callback != NULL) {
                    const at_token_t token = {
                        .type = AT_TOKEN_LINE_OVERFLOW,
                        .text = NULL,
                        .length = 0,
                    };
                    callback(&token, user_ctx);
                }
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            emit_line(parser, callback, user_ctx);
            continue;
        }

        /* GSM modems commonly emit the SMS data prompt as CRLF + "> ". */
        if (parser->prompt_enabled && ch == '>' && parser->line_length == 0) {
            if (callback != NULL) {
                const at_token_t token = {
                    .type = AT_TOKEN_PROMPT,
                    .text = ">",
                    .length = 1,
                };
                callback(&token, user_ctx);
            }
            parser->skip_prompt_space = true;
            continue;
        }

        if (parser->line_length >= AT_PARSER_MAX_LINE_LENGTH) {
            parser->discarding_overflow = true;
            parser->line_length = 0;
            continue;
        }

        parser->line[parser->line_length++] = ch;
    }
}
