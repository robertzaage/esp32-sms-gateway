#include "sms_gsm7.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* GSM 03.38 default alphabet represented as Unicode code points. */
static const uint32_t s_gsm7_basic[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

typedef struct {
    uint32_t codepoint;
    uint8_t extension;
} gsm7_extension_t;

static const gsm7_extension_t s_gsm7_extension[] = {
    {0x000C, 0x0A}, /* form feed */
    {0x005E, 0x14},
    {0x007B, 0x28},
    {0x007D, 0x29},
    {0x005C, 0x2F},
    {0x005B, 0x3C},
    {0x007E, 0x3D},
    {0x005D, 0x3E},
    {0x007C, 0x40},
    {0x20AC, 0x65},
};

bool sms_utf8_next(const char **cursor, uint32_t *codepoint)
{
    if (cursor == NULL || *cursor == NULL || codepoint == NULL || **cursor == '\0') {
        return false;
    }

    const uint8_t *p = (const uint8_t *)*cursor;
    uint32_t cp;
    size_t count;

    if (p[0] < 0x80) {
        cp = p[0];
        count = 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) {
            return false;
        }
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        if (cp < 0x80) {
            return false;
        }
        count = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
            return false;
        }
        cp = ((uint32_t)(p[0] & 0x0F) << 12) |
             ((uint32_t)(p[1] & 0x3F) << 6) |
             (uint32_t)(p[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        count = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) {
            return false;
        }
        cp = ((uint32_t)(p[0] & 0x07) << 18) |
             ((uint32_t)(p[1] & 0x3F) << 12) |
             ((uint32_t)(p[2] & 0x3F) << 6) |
             (uint32_t)(p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) {
            return false;
        }
        count = 4;
    } else {
        return false;
    }

    *codepoint = cp;
    *cursor += count;
    return true;
}

bool sms_utf8_append(uint32_t codepoint, char *buffer, size_t capacity, size_t *length)
{
    if (buffer == NULL || length == NULL || *length >= capacity) {
        return false;
    }

    uint8_t out[4];
    size_t count;
    if (codepoint <= 0x7F) {
        out[0] = (uint8_t)codepoint;
        count = 1;
    } else if (codepoint <= 0x7FF) {
        out[0] = (uint8_t)(0xC0 | (codepoint >> 6));
        out[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        out[0] = (uint8_t)(0xE0 | (codepoint >> 12));
        out[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 3;
    } else if (codepoint <= 0x10FFFF) {
        out[0] = (uint8_t)(0xF0 | (codepoint >> 18));
        out[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 4;
    } else {
        return false;
    }

    if (count + *length >= capacity) {
        return false;
    }
    memcpy(buffer + *length, out, count);
    *length += count;
    buffer[*length] = '\0';
    return true;
}

static bool gsm7_find(uint32_t codepoint, uint8_t *first, uint8_t *second, size_t *count)
{
    for (size_t i = 0; i < 128; ++i) {
        if (i != 0x1B && s_gsm7_basic[i] == codepoint) {
            *first = (uint8_t)i;
            *count = 1;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(s_gsm7_extension) / sizeof(s_gsm7_extension[0]); ++i) {
        if (s_gsm7_extension[i].codepoint == codepoint) {
            *first = 0x1B;
            *second = s_gsm7_extension[i].extension;
            *count = 2;
            return true;
        }
    }
    return false;
}

size_t sms_gsm7_septet_count(const char *utf8_text)
{
    if (utf8_text == NULL) {
        return SIZE_MAX;
    }
    size_t total = 0;
    const char *cursor = utf8_text;
    while (*cursor != '\0') {
        uint32_t cp;
        if (!sms_utf8_next(&cursor, &cp)) {
            return SIZE_MAX;
        }
        uint8_t first = 0;
        uint8_t second = 0;
        size_t count = 0;
        if (!gsm7_find(cp, &first, &second, &count)) {
            return SIZE_MAX;
        }
        if (SIZE_MAX - total < count) {
            return SIZE_MAX;
        }
        total += count;
    }
    return total;
}

bool sms_gsm7_encode(const char *utf8_text,
                     uint8_t *septets,
                     size_t septet_capacity,
                     size_t *septet_count)
{
    if (utf8_text == NULL || septet_count == NULL || (septet_capacity > 0 && septets == NULL)) {
        return false;
    }

    size_t out = 0;
    const char *cursor = utf8_text;
    while (*cursor != '\0') {
        uint32_t cp;
        if (!sms_utf8_next(&cursor, &cp)) {
            return false;
        }
        uint8_t first = 0;
        uint8_t second = 0;
        size_t count = 0;
        if (!gsm7_find(cp, &first, &second, &count) || out + count > septet_capacity) {
            return false;
        }
        septets[out++] = first;
        if (count == 2) {
            septets[out++] = second;
        }
    }
    *septet_count = out;
    return true;
}

static bool gsm7_extension_decode(uint8_t value, uint32_t *codepoint)
{
    for (size_t i = 0; i < sizeof(s_gsm7_extension) / sizeof(s_gsm7_extension[0]); ++i) {
        if (s_gsm7_extension[i].extension == value) {
            *codepoint = s_gsm7_extension[i].codepoint;
            return true;
        }
    }
    return false;
}

bool sms_gsm7_decode(const uint8_t *septets,
                     size_t septet_count,
                     char *utf8,
                     size_t utf8_capacity)
{
    if ((septet_count > 0 && septets == NULL) || utf8 == NULL || utf8_capacity == 0) {
        return false;
    }
    size_t out = 0;
    utf8[0] = '\0';
    for (size_t i = 0; i < septet_count; ++i) {
        uint32_t cp;
        if (septets[i] == 0x1B) {
            if (++i >= septet_count || !gsm7_extension_decode(septets[i], &cp)) {
                return false;
            }
        } else {
            cp = s_gsm7_basic[septets[i] & 0x7F];
        }
        if (!sms_utf8_append(cp, utf8, utf8_capacity, &out)) {
            return false;
        }
    }
    return true;
}
