#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of GSM 03.38 septets required, or SIZE_MAX if not representable. */
size_t sms_gsm7_septet_count(const char *utf8_text);

/** Encode UTF-8 to GSM 03.38 septets (ESC extension consumes two septets). */
bool sms_gsm7_encode(const char *utf8_text,
                     uint8_t *septets,
                     size_t septet_capacity,
                     size_t *septet_count);

/** Decode GSM septets to UTF-8. */
bool sms_gsm7_decode(const uint8_t *septets,
                     size_t septet_count,
                     char *utf8,
                     size_t utf8_capacity);

/** Decode one UTF-8 code point, advancing *cursor. */
bool sms_utf8_next(const char **cursor, uint32_t *codepoint);

/** Append one Unicode scalar as UTF-8, updating *length. */
bool sms_utf8_append(uint32_t codepoint, char *buffer, size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif
