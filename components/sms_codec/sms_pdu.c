#include "sms_codec.h"
#include "sms_gsm7.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMS_MAX_GSM7_SEPTETS (SMS_MAX_SEGMENTS * 160U)
#define SMS_MAX_UCS2_UNITS   (SMS_MAX_SEGMENTS * 70U)

typedef struct {
    uint8_t bytes[SMS_MAX_PDU_OCTETS];
    size_t length;
} pdu_buffer_t;

static bool unpack_septets(const uint8_t *packed,
                           size_t packed_len,
                           size_t start_bit,
                           size_t count,
                           uint8_t *septets);

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static sms_pdu_decode_result_t hex_decode(const char *hex, uint8_t *out, size_t out_capacity, size_t *out_len)
{
    if (hex == NULL || out == NULL || out_len == NULL) {
        return SMS_PDU_DECODE_INVALID;
    }
    const size_t length = strlen(hex);
    if ((length & 1U) != 0U) {
        return SMS_PDU_DECODE_INVALID_HEX;
    }
    if (length / 2U > out_capacity) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    for (size_t i = 0; i < length; i += 2) {
        const int high = hex_value(hex[i]);
        const int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return SMS_PDU_DECODE_INVALID_HEX;
        }
        out[i / 2U] = (uint8_t)((high << 4) | low);
    }
    *out_len = length / 2U;
    return SMS_PDU_DECODE_OK;
}

static bool hex_encode(const uint8_t *data, size_t data_len, char *out, size_t out_capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    if (data == NULL || out == NULL || out_capacity < data_len * 2U + 1U) {
        return false;
    }
    for (size_t i = 0; i < data_len; ++i) {
        out[i * 2U] = hex[data[i] >> 4];
        out[i * 2U + 1U] = hex[data[i] & 0x0F];
    }
    out[data_len * 2U] = '\0';
    return true;
}

static bool buffer_put(pdu_buffer_t *buffer, uint8_t byte)
{
    if (buffer == NULL || buffer->length >= sizeof(buffer->bytes)) {
        return false;
    }
    buffer->bytes[buffer->length++] = byte;
    return true;
}

static bool buffer_append(pdu_buffer_t *buffer, const uint8_t *data, size_t length)
{
    if (buffer == NULL || (length > 0 && data == NULL) || buffer->length + length > sizeof(buffer->bytes)) {
        return false;
    }
    memcpy(buffer->bytes + buffer->length, data, length);
    buffer->length += length;
    return true;
}

static bool address_validate(const char *address, const char **digits_out, size_t *digits_len, bool *international)
{
    if (address == NULL || digits_out == NULL || digits_len == NULL || international == NULL) {
        return false;
    }
    const char *digits = address;
    bool intl = false;
    if (*digits == '+') {
        intl = true;
        ++digits;
    }
    const size_t length = strlen(digits);
    if (length == 0 || length > 20) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!isdigit((unsigned char)digits[i])) {
            return false;
        }
    }
    *digits_out = digits;
    *digits_len = length;
    *international = intl;
    return true;
}

static bool address_encode(pdu_buffer_t *buffer, const char *address)
{
    const char *digits;
    size_t digits_len;
    bool international;
    if (!address_validate(address, &digits, &digits_len, &international)) {
        return false;
    }
    if (!buffer_put(buffer, (uint8_t)digits_len) ||
        !buffer_put(buffer, international ? 0x91 : 0x81)) {
        return false;
    }
    for (size_t i = 0; i < digits_len; i += 2) {
        const uint8_t low = (uint8_t)(digits[i] - '0');
        const uint8_t high = (i + 1 < digits_len) ? (uint8_t)(digits[i + 1] - '0') : 0x0F;
        if (!buffer_put(buffer, (uint8_t)(low | (high << 4)))) {
            return false;
        }
    }
    return true;
}

static bool address_decode(const uint8_t *data,
                           size_t data_len,
                           size_t semi_octet_count,
                           uint8_t toa,
                           char *out,
                           size_t out_capacity)
{
    if (data == NULL || out == NULL || out_capacity == 0) {
        return false;
    }
    const size_t required_bytes = (semi_octet_count + 1U) / 2U;
    if (data_len < required_bytes) {
        return false;
    }

    const uint8_t ton = (toa >> 4) & 0x07U;
    if (ton == 0x05U) {
        /* Alphanumeric TP address: Address-Length is still in useful semi-octets. */
        const size_t septet_count = (semi_octet_count * 4U) / 7U;
        if (septet_count > 11U) {
            return false;
        }
        uint8_t septets[11];
        if (!unpack_septets(data, required_bytes, 0, septet_count, septets)) {
            return false;
        }
        return sms_gsm7_decode(septets, septet_count, out, out_capacity);
    }

    size_t pos = 0;
    if (ton == 0x01U) {
        if (pos + 1 >= out_capacity) {
            return false;
        }
        out[pos++] = '+';
    }
    for (size_t i = 0; i < semi_octet_count; ++i) {
        const uint8_t nibble = (i & 1U) == 0U ? (data[i / 2U] & 0x0FU) : (data[i / 2U] >> 4);
        char ch;
        if (nibble <= 9U) {
            ch = (char)('0' + nibble);
        } else if (nibble == 0x0AU) {
            ch = '*';
        } else if (nibble == 0x0BU) {
            ch = '#';
        } else if (nibble >= 0x0CU && nibble <= 0x0EU) {
            ch = (char)('a' + (nibble - 0x0CU));
        } else {
            return false;
        }
        if (pos + 1 >= out_capacity) {
            return false;
        }
        out[pos++] = ch;
    }
    out[pos] = '\0';
    return true;
}

static size_t concat_header(uint16_t reference, uint8_t total, uint8_t sequence, uint8_t out[7])
{
    if (reference <= 0xFFU) {
        out[0] = 0x05;
        out[1] = 0x00;
        out[2] = 0x03;
        out[3] = (uint8_t)reference;
        out[4] = total;
        out[5] = sequence;
        return 6;
    }
    out[0] = 0x06;
    out[1] = 0x08;
    out[2] = 0x04;
    out[3] = (uint8_t)(reference >> 8);
    out[4] = (uint8_t)reference;
    out[5] = total;
    out[6] = sequence;
    return 7;
}

static size_t header_septets(size_t header_bytes)
{
    return (header_bytes * 8U + 6U) / 7U;
}

static bool pack_septets(const uint8_t *septets,
                         size_t septet_count,
                         const uint8_t *udh,
                         size_t udh_len,
                         uint8_t *out,
                         size_t out_capacity,
                         size_t *out_bytes,
                         uint8_t *udl)
{
    if ((septet_count > 0 && septets == NULL) || out == NULL || out_bytes == NULL || udl == NULL) {
        return false;
    }
    const size_t hseptets = udh_len > 0 ? header_septets(udh_len) : 0;
    const size_t total_septets = hseptets + septet_count;
    const size_t total_bytes = (total_septets * 7U + 7U) / 8U;
    if (total_septets > 255U || total_bytes > out_capacity) {
        return false;
    }
    memset(out, 0, total_bytes);
    if (udh_len > 0) {
        memcpy(out, udh, udh_len);
    }
    size_t bit = hseptets * 7U;
    for (size_t i = 0; i < septet_count; ++i) {
        const uint8_t value = septets[i] & 0x7FU;
        const size_t byte_index = bit / 8U;
        const unsigned shift = (unsigned)(bit & 7U);
        out[byte_index] |= (uint8_t)(value << shift);
        if (shift > 1U && byte_index + 1U < total_bytes) {
            out[byte_index + 1U] |= (uint8_t)(value >> (8U - shift));
        }
        bit += 7U;
    }
    *out_bytes = total_bytes;
    *udl = (uint8_t)total_septets;
    return true;
}

static bool unpack_septets(const uint8_t *packed,
                           size_t packed_len,
                           size_t start_bit,
                           size_t count,
                           uint8_t *septets)
{
    if (packed == NULL || (count > 0 && septets == NULL)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t bit = start_bit + i * 7U;
        const size_t byte_index = bit / 8U;
        const unsigned shift = (unsigned)(bit & 7U);
        if (byte_index >= packed_len) {
            return false;
        }
        uint16_t value = (uint16_t)(packed[byte_index] >> shift);
        if (shift > 1U) {
            if (byte_index + 1U >= packed_len) {
                return false;
            }
            value |= (uint16_t)packed[byte_index + 1U] << (8U - shift);
        }
        septets[i] = (uint8_t)(value & 0x7FU);
    }
    return true;
}

static bool build_submit_pdu(const char *recipient,
                             sms_encoding_t encoding,
                             bool request_status_report,
                             const uint8_t *udh,
                             size_t udh_len,
                             const uint8_t *payload,
                             size_t payload_count,
                             sms_submit_segment_t *segment)
{
    pdu_buffer_t pdu = {0};
    if (!buffer_put(&pdu, 0x00)) { /* SMSC: use modem/network default */
        return false;
    }
    const size_t tpdu_start = pdu.length;
    uint8_t first = 0x01; /* SMS-SUBMIT */
    if (request_status_report) {
        first |= 0x20;
    }
    if (udh_len > 0) {
        first |= 0x40;
    }
    if (!buffer_put(&pdu, first) || !buffer_put(&pdu, 0x00) || !address_encode(&pdu, recipient) ||
        !buffer_put(&pdu, 0x00)) { /* PID */
        return false;
    }

    if (encoding == SMS_ENCODING_GSM7) {
        if (!buffer_put(&pdu, 0x00)) {
            return false;
        }
        uint8_t packed[140] = {0};
        size_t packed_len = 0;
        uint8_t udl = 0;
        if (!pack_septets(payload, payload_count, udh, udh_len,
                          packed, sizeof(packed), &packed_len, &udl) ||
            !buffer_put(&pdu, udl) || !buffer_append(&pdu, packed, packed_len)) {
            return false;
        }
    } else if (encoding == SMS_ENCODING_UCS2) {
        if (!buffer_put(&pdu, 0x08)) {
            return false;
        }
        const size_t ud_octets = udh_len + payload_count;
        if (ud_octets > 140U || !buffer_put(&pdu, (uint8_t)ud_octets) ||
            (udh_len > 0 && !buffer_append(&pdu, udh, udh_len)) ||
            !buffer_append(&pdu, payload, payload_count)) {
            return false;
        }
    } else {
        return false;
    }

    segment->encoding = encoding;
    segment->tpdu_length_octets = pdu.length - tpdu_start;
    return hex_encode(pdu.bytes, pdu.length, segment->pdu_hex, sizeof(segment->pdu_hex));
}

static bool split_gsm7(const uint8_t *septets,
                       size_t septet_count,
                       size_t per_segment,
                       size_t offsets[SMS_MAX_SEGMENTS + 1],
                       size_t *count)
{
    size_t pos = 0;
    size_t parts = 0;
    offsets[0] = 0;
    while (pos < septet_count) {
        if (parts >= SMS_MAX_SEGMENTS) {
            return false;
        }
        size_t take = septet_count - pos;
        if (take > per_segment) {
            take = per_segment;
            if (take > 0 && septets[pos + take - 1U] == 0x1B) {
                --take;
            }
        }
        if (take == 0) {
            return false;
        }
        pos += take;
        offsets[++parts] = pos;
    }
    *count = parts;
    return true;
}

static bool utf8_to_ucs2(const char *text, uint16_t *units, size_t capacity, size_t *count)
{
    if (text == NULL || units == NULL || count == NULL) {
        return false;
    }
    size_t out = 0;
    const char *cursor = text;
    while (*cursor != '\0') {
        uint32_t cp;
        if (!sms_utf8_next(&cursor, &cp)) {
            return false;
        }
        if (cp <= 0xFFFFU) {
            if ((cp >= 0xD800U && cp <= 0xDFFFU) || out >= capacity) {
                return false;
            }
            units[out++] = (uint16_t)cp;
        } else {
            /* In practice SMS implementations commonly transport UTF-16BE surrogate pairs under DCS UCS-2. */
            if (cp > 0x10FFFFU || out + 2U > capacity) {
                return false;
            }
            cp -= 0x10000U;
            units[out++] = (uint16_t)(0xD800U | (cp >> 10));
            units[out++] = (uint16_t)(0xDC00U | (cp & 0x03FFU));
        }
    }
    *count = out;
    return true;
}

bool sms_submit_encode(const char *recipient,
                       const char *utf8_text,
                       bool request_status_report,
                       uint16_t concat_reference,
                       sms_submit_segment_t *segments,
                       size_t max_segments,
                       size_t *segment_count)
{
    if (recipient == NULL || utf8_text == NULL || segments == NULL || segment_count == NULL || max_segments == 0) {
        return false;
    }
    const char *digits;
    size_t digits_len;
    bool intl;
    if (!address_validate(recipient, &digits, &digits_len, &intl)) {
        return false;
    }
    (void)digits;
    (void)digits_len;
    (void)intl;

    const size_t gsm_count = sms_gsm7_septet_count(utf8_text);
    if (gsm_count != SIZE_MAX) {
        if (gsm_count > SMS_MAX_GSM7_SEPTETS) {
            return false;
        }
        uint8_t septets[SMS_MAX_GSM7_SEPTETS];
        size_t encoded_count = 0;
        if (!sms_gsm7_encode(utf8_text, septets, sizeof(septets), &encoded_count)) {
            return false;
        }
        if (encoded_count <= 160U) {
            if (max_segments < 1 || !build_submit_pdu(recipient, SMS_ENCODING_GSM7,
                                                      request_status_report, NULL, 0,
                                                      septets, encoded_count, &segments[0])) {
                return false;
            }
            segments[0].segment_number = 1;
            segments[0].segment_count = 1;
            segments[0].concat.present = false;
            *segment_count = 1;
            return true;
        }

        uint8_t dummy_udh[7];
        const size_t udh_len = concat_header(concat_reference, 1, 1, dummy_udh);
        const size_t capacity = 160U - header_septets(udh_len);
        size_t offsets[SMS_MAX_SEGMENTS + 1] = {0};
        size_t parts = 0;
        if (!split_gsm7(septets, encoded_count, capacity, offsets, &parts) || parts > max_segments || parts > 255U) {
            return false;
        }
        for (size_t i = 0; i < parts; ++i) {
            uint8_t udh[7];
            const size_t current_udh_len = concat_header(concat_reference, (uint8_t)parts, (uint8_t)(i + 1U), udh);
            const size_t start = offsets[i];
            const size_t count = offsets[i + 1U] - start;
            if (!build_submit_pdu(recipient, SMS_ENCODING_GSM7, request_status_report,
                                  udh, current_udh_len, septets + start, count, &segments[i])) {
                return false;
            }
            segments[i].segment_number = (uint8_t)(i + 1U);
            segments[i].segment_count = (uint8_t)parts;
            segments[i].concat.present = true;
            segments[i].concat.is_16bit = concat_reference > 0xFFU;
            segments[i].concat.reference = concat_reference;
            segments[i].concat.total_parts = (uint8_t)parts;
            segments[i].concat.part_number = (uint8_t)(i + 1U);
        }
        *segment_count = parts;
        return true;
    }

    uint16_t units[SMS_MAX_UCS2_UNITS];
    size_t unit_count = 0;
    if (!utf8_to_ucs2(utf8_text, units, SMS_MAX_UCS2_UNITS, &unit_count)) {
        return false;
    }
    if (unit_count <= 70U) {
        uint8_t payload[140];
        for (size_t i = 0; i < unit_count; ++i) {
            payload[i * 2U] = (uint8_t)(units[i] >> 8);
            payload[i * 2U + 1U] = (uint8_t)units[i];
        }
        if (!build_submit_pdu(recipient, SMS_ENCODING_UCS2, request_status_report,
                              NULL, 0, payload, unit_count * 2U, &segments[0])) {
            return false;
        }
        segments[0].segment_number = 1;
        segments[0].segment_count = 1;
        segments[0].concat.present = false;
        *segment_count = 1;
        return true;
    }

    uint8_t dummy_udh[7];
    const size_t udh_len = concat_header(concat_reference, 1, 1, dummy_udh);
    const size_t units_per_segment = (140U - udh_len) / 2U;
    size_t offsets[SMS_MAX_SEGMENTS + 1] = {0};
    size_t parts = 0;
    size_t cursor = 0;
    offsets[0] = 0;
    while (cursor < unit_count) {
        if (parts >= SMS_MAX_SEGMENTS) {
            return false;
        }
        size_t count = unit_count - cursor;
        if (count > units_per_segment) {
            count = units_per_segment;
        }
        if (count > 0 && cursor + count < unit_count &&
            units[cursor + count - 1U] >= 0xD800U && units[cursor + count - 1U] <= 0xDBFFU) {
            --count;
        }
        if (count == 0) {
            return false;
        }
        cursor += count;
        offsets[++parts] = cursor;
    }
    if (parts == 0 || parts > max_segments || parts > 255U) {
        return false;
    }
    for (size_t i = 0; i < parts; ++i) {
        const size_t start = offsets[i];
        const size_t count = offsets[i + 1U] - start;
        uint8_t payload[140];
        for (size_t j = 0; j < count; ++j) {
            payload[j * 2U] = (uint8_t)(units[start + j] >> 8);
            payload[j * 2U + 1U] = (uint8_t)units[start + j];
        }
        uint8_t udh[7];
        const size_t current_udh_len = concat_header(concat_reference, (uint8_t)parts, (uint8_t)(i + 1U), udh);
        if (!build_submit_pdu(recipient, SMS_ENCODING_UCS2, request_status_report,
                              udh, current_udh_len, payload, count * 2U, &segments[i])) {
            return false;
        }
        segments[i].segment_number = (uint8_t)(i + 1U);
        segments[i].segment_count = (uint8_t)parts;
        segments[i].concat.present = true;
        segments[i].concat.is_16bit = concat_reference > 0xFFU;
        segments[i].concat.reference = concat_reference;
        segments[i].concat.total_parts = (uint8_t)parts;
        segments[i].concat.part_number = (uint8_t)(i + 1U);
    }
    *segment_count = parts;
    return true;
}

bool sms_submit_preflight(const char *recipient,
                          const char *utf8_text,
                          uint16_t concat_reference,
                          sms_encoding_t *encoding,
                          size_t *segment_count)
{
    if (recipient == NULL || utf8_text == NULL || encoding == NULL || segment_count == NULL) {
        return false;
    }
    *encoding = SMS_ENCODING_UNKNOWN;
    *segment_count = 0;

    const char *digits = NULL;
    size_t digits_len = 0;
    bool intl = false;
    if (!address_validate(recipient, &digits, &digits_len, &intl)) {
        return false;
    }
    (void)digits; (void)digits_len; (void)intl;

    const size_t gsm_count = sms_gsm7_septet_count(utf8_text);
    if (gsm_count != SIZE_MAX) {
        if (gsm_count > SMS_MAX_GSM7_SEPTETS) return false;
        if (gsm_count <= 160U) {
            *encoding = SMS_ENCODING_GSM7;
            *segment_count = 1;
            return true;
        }
        uint8_t septets[SMS_MAX_GSM7_SEPTETS];
        size_t encoded_count = 0;
        if (!sms_gsm7_encode(utf8_text, septets, sizeof(septets), &encoded_count)) return false;
        uint8_t dummy_udh[7];
        const size_t udh_len = concat_header(concat_reference, 1, 1, dummy_udh);
        const size_t capacity = 160U - header_septets(udh_len);
        size_t offsets[SMS_MAX_SEGMENTS + 1] = {0};
        size_t parts = 0;
        if (!split_gsm7(septets, encoded_count, capacity, offsets, &parts) || parts == 0 || parts > SMS_MAX_SEGMENTS) {
            return false;
        }
        *encoding = SMS_ENCODING_GSM7;
        *segment_count = parts;
        return true;
    }

    uint16_t units[SMS_MAX_UCS2_UNITS];
    size_t unit_count = 0;
    if (!utf8_to_ucs2(utf8_text, units, SMS_MAX_UCS2_UNITS, &unit_count)) return false;
    if (unit_count <= 70U) {
        *encoding = SMS_ENCODING_UCS2;
        *segment_count = 1;
        return true;
    }
    uint8_t dummy_udh[7];
    const size_t udh_len = concat_header(concat_reference, 1, 1, dummy_udh);
    const size_t units_per_segment = (140U - udh_len) / 2U;
    size_t cursor = 0;
    size_t parts = 0;
    while (cursor < unit_count) {
        if (parts >= SMS_MAX_SEGMENTS) return false;
        size_t count = unit_count - cursor;
        if (count > units_per_segment) count = units_per_segment;
        if (count > 0 && cursor + count < unit_count &&
            units[cursor + count - 1U] >= 0xD800U && units[cursor + count - 1U] <= 0xDBFFU) {
            --count;
        }
        if (count == 0) return false;
        cursor += count;
        ++parts;
    }
    if (parts == 0 || parts > SMS_MAX_SEGMENTS) return false;
    *encoding = SMS_ENCODING_UCS2;
    *segment_count = parts;
    return true;
}

static sms_encoding_t dcs_encoding(uint8_t dcs)
{
    if ((dcs & 0xC0U) == 0x00U) {
        if ((dcs & 0x20U) != 0U) return SMS_ENCODING_UNKNOWN; /* compressed TP-UD unsupported */
        switch ((dcs >> 2) & 0x03U) {
        case 0:
            return SMS_ENCODING_GSM7;
        case 1:
            return SMS_ENCODING_8BIT;
        case 2:
            return SMS_ENCODING_UCS2;
        default:
            return SMS_ENCODING_UNKNOWN;
        }
    }
    if ((dcs & 0xF0U) == 0xF0U) {
        return (dcs & 0x04U) != 0U ? SMS_ENCODING_8BIT : SMS_ENCODING_GSM7;
    }
    return SMS_ENCODING_UNKNOWN;
}

static bool parse_concat_udh(const uint8_t *udh, size_t udh_len, sms_concat_info_t *concat)
{
    if (concat == NULL) {
        return false;
    }
    memset(concat, 0, sizeof(*concat));
    if (udh == NULL || udh_len == 0) {
        return true;
    }
    if ((size_t)udh[0] + 1U > udh_len) {
        return false;
    }
    const size_t end = (size_t)udh[0] + 1U;
    size_t pos = 1;
    while (pos + 2U <= end) {
        const uint8_t iei = udh[pos++];
        const uint8_t len = udh[pos++];
        if (pos + len > end) {
            return false;
        }
        if (iei == 0x00 && len == 3) {
            concat->present = true;
            concat->is_16bit = false;
            concat->reference = udh[pos];
            concat->total_parts = udh[pos + 1U];
            concat->part_number = udh[pos + 2U];
        } else if (iei == 0x08 && len == 4) {
            concat->present = true;
            concat->is_16bit = true;
            concat->reference = (uint16_t)(((uint16_t)udh[pos] << 8) | udh[pos + 1U]);
            concat->total_parts = udh[pos + 2U];
            concat->part_number = udh[pos + 3U];
        }
        pos += len;
    }
    if (concat->present &&
        (concat->total_parts == 0 || concat->part_number == 0 || concat->part_number > concat->total_parts)) {
        return false;
    }
    return true;
}

static bool udh_has_national_language_shift(const uint8_t *udh, size_t udh_len)
{
    if (udh == NULL || udh_len == 0 || (size_t)udh[0] + 1U > udh_len) return false;
    const size_t end = (size_t)udh[0] + 1U;
    size_t pos = 1;
    while (pos + 2U <= end) {
        const uint8_t iei = udh[pos++];
        const uint8_t len = udh[pos++];
        if (pos + len > end) return false;
        if (iei == 0x24U || iei == 0x25U) return true;
        pos += len;
    }
    return false;
}

static int semi_octet_value(uint8_t byte)
{
    const int tens = byte & 0x0F;
    const int ones = (byte >> 4) & 0x0F;
    if (tens > 9 || ones > 9) {
        return -1;
    }
    return tens * 10 + ones;
}

static bool timestamp_decode(const uint8_t *data, size_t data_len, char *out, size_t out_capacity)
{
    if (data == NULL || data_len < 7 || out == NULL || out_capacity < 24) {
        return false;
    }
    const int yy = semi_octet_value(data[0]);
    const int month = semi_octet_value(data[1]);
    const int day = semi_octet_value(data[2]);
    const int hour = semi_octet_value(data[3]);
    const int minute = semi_octet_value(data[4]);
    const int second = semi_octet_value(data[5]);
    const bool negative = (data[6] & 0x08U) != 0U;
    const int quarters = (data[6] & 0x07U) * 10 + ((data[6] >> 4) & 0x0FU);
    if (yy < 0 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
        quarters < 0 || quarters > 79) {
        return false;
    }
    const int tz_minutes = quarters * 15;
    const int tz_hours = tz_minutes / 60;
    const int tz_mins = tz_minutes % 60;
    /* TP-SCTS has a two-digit year. Use the conventional 1970/2069 pivot. */
    const int year = yy >= 70 ? 1900 + yy : 2000 + yy;
    const int written = snprintf(out, out_capacity,
                                 "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                                 year, month, day, hour, minute, second,
                                 negative ? '-' : '+', tz_hours, tz_mins);
    return written > 0 && (size_t)written < out_capacity;
}

static sms_pdu_decode_result_t decode_user_data(uint8_t first_octet,
                                                uint8_t dcs,
                                                uint8_t udl,
                                                const uint8_t *ud,
                                                size_t ud_available,
                                                sms_encoding_t *encoding,
                                                sms_concat_info_t *concat,
                                                char *text,
                                                size_t text_capacity)
{
    if (encoding == NULL || concat == NULL || text == NULL || text_capacity == 0) {
        return SMS_PDU_DECODE_INVALID;
    }
    *encoding = dcs_encoding(dcs);
    memset(concat, 0, sizeof(*concat));
    text[0] = '\0';
    const bool has_udh = (first_octet & 0x40U) != 0U;

    if (*encoding == SMS_ENCODING_GSM7) {
        const size_t packed_bytes = ((size_t)udl * 7U + 7U) / 8U;
        if (packed_bytes > ud_available || udl > 160U) {
            return SMS_PDU_DECODE_TRUNCATED;
        }
        size_t hseptets = 0;
        size_t start_bit = 0;
        if (has_udh) {
            if (packed_bytes == 0) {
                return SMS_PDU_DECODE_INVALID;
            }
            const size_t header_bytes = (size_t)ud[0] + 1U;
            if (header_bytes > packed_bytes || !parse_concat_udh(ud, header_bytes, concat)) {
                return SMS_PDU_DECODE_INVALID;
            }
            if (udh_has_national_language_shift(ud, header_bytes)) return SMS_PDU_DECODE_UNSUPPORTED;
            hseptets = header_septets(header_bytes);
            if (hseptets > udl) {
                return SMS_PDU_DECODE_INVALID;
            }
            start_bit = hseptets * 7U;
        }
        const size_t text_septets = (size_t)udl - hseptets;
        uint8_t septets[160];
        if (!unpack_septets(ud, packed_bytes, start_bit, text_septets, septets)) {
            return SMS_PDU_DECODE_TRUNCATED;
        }
        if (!sms_gsm7_decode(septets, text_septets, text, text_capacity)) {
            return SMS_PDU_DECODE_TEXT_TOO_LONG;
        }
        return SMS_PDU_DECODE_OK;
    }

    if (*encoding == SMS_ENCODING_UCS2) {
        if ((size_t)udl > ud_available || udl > 140U) {
            return SMS_PDU_DECODE_TRUNCATED;
        }
        size_t pos = 0;
        if (has_udh) {
            if (udl == 0) {
                return SMS_PDU_DECODE_INVALID;
            }
            const size_t header_bytes = (size_t)ud[0] + 1U;
            if (header_bytes > udl || !parse_concat_udh(ud, header_bytes, concat)) {
                return SMS_PDU_DECODE_INVALID;
            }
            if (udh_has_national_language_shift(ud, header_bytes)) return SMS_PDU_DECODE_UNSUPPORTED;
            pos = header_bytes;
        }
        if (((size_t)udl - pos) % 2U != 0U) {
            return SMS_PDU_DECODE_INVALID;
        }
        size_t out_len = 0;
        while (pos + 1U < udl) {
            uint32_t cp = ((uint32_t)ud[pos] << 8) | ud[pos + 1U];
            pos += 2U;
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                if (pos + 1U >= udl) {
                    return SMS_PDU_DECODE_INVALID;
                }
                const uint32_t low = ((uint32_t)ud[pos] << 8) | ud[pos + 1U];
                if (low < 0xDC00U || low > 0xDFFFU) {
                    return SMS_PDU_DECODE_INVALID;
                }
                pos += 2U;
                cp = 0x10000U + ((cp - 0xD800U) << 10) + (low - 0xDC00U);
            } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                return SMS_PDU_DECODE_INVALID;
            }
            if (!sms_utf8_append(cp, text, text_capacity, &out_len)) {
                return SMS_PDU_DECODE_TEXT_TOO_LONG;
            }
        }
        return SMS_PDU_DECODE_OK;
    }

    if (*encoding == SMS_ENCODING_8BIT) {
        return SMS_PDU_DECODE_UNSUPPORTED;
    }
    return SMS_PDU_DECODE_UNSUPPORTED;
}

sms_pdu_type_t sms_pdu_type_detect(const char *pdu_hex)
{
    uint8_t pdu[SMS_MAX_PDU_OCTETS];
    size_t pdu_len = 0;
    if (hex_decode(pdu_hex, pdu, sizeof(pdu), &pdu_len) != SMS_PDU_DECODE_OK || pdu_len < 2) {
        return SMS_PDU_TYPE_UNKNOWN;
    }
    const size_t smsc_len = pdu[0];
    if (1U + smsc_len >= pdu_len) {
        return SMS_PDU_TYPE_UNKNOWN;
    }
    switch (pdu[1U + smsc_len] & 0x03U) {
    case 0x00: return SMS_PDU_TYPE_DELIVER;
    case 0x02: return SMS_PDU_TYPE_STATUS_REPORT;
    default: return SMS_PDU_TYPE_UNKNOWN;
    }
}

sms_pdu_decode_result_t sms_deliver_decode(const char *pdu_hex, sms_deliver_t *out)
{
    if (pdu_hex == NULL || out == NULL) {
        return SMS_PDU_DECODE_INVALID;
    }
    uint8_t pdu[SMS_MAX_PDU_OCTETS];
    size_t pdu_len = 0;
    sms_pdu_decode_result_t result = hex_decode(pdu_hex, pdu, sizeof(pdu), &pdu_len);
    if (result != SMS_PDU_DECODE_OK) {
        return result;
    }
    memset(out, 0, sizeof(*out));
    if (pdu_len < 2) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    size_t pos = 0;
    const size_t smsc_len = pdu[pos++];
    if (pos + smsc_len >= pdu_len) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    pos += smsc_len;
    const uint8_t first = pdu[pos++];
    if ((first & 0x03U) != 0x00U || pos + 2 > pdu_len) {
        return SMS_PDU_DECODE_UNSUPPORTED;
    }
    const size_t oa_digits = pdu[pos++];
    const uint8_t oa_toa = pdu[pos++];
    const size_t oa_bytes = (oa_digits + 1U) / 2U;
    if (pos + oa_bytes + 10U > pdu_len ||
        !address_decode(pdu + pos, pdu_len - pos, oa_digits, oa_toa,
                        out->sender, sizeof(out->sender))) {
        return SMS_PDU_DECODE_INVALID;
    }
    pos += oa_bytes;
    ++pos; /* PID */
    const uint8_t dcs = pdu[pos++];
    if (!timestamp_decode(pdu + pos, pdu_len - pos, out->service_center_timestamp,
                          sizeof(out->service_center_timestamp))) {
        return SMS_PDU_DECODE_INVALID;
    }
    pos += 7;
    if (pos >= pdu_len) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    const uint8_t udl = pdu[pos++];
    return decode_user_data(first, dcs, udl, pdu + pos, pdu_len - pos,
                            &out->encoding, &out->concat, out->text, sizeof(out->text));
}

sms_pdu_decode_result_t sms_status_report_decode(const char *pdu_hex, sms_status_report_t *out)
{
    if (pdu_hex == NULL || out == NULL) {
        return SMS_PDU_DECODE_INVALID;
    }
    uint8_t pdu[SMS_MAX_PDU_OCTETS];
    size_t pdu_len = 0;
    sms_pdu_decode_result_t result = hex_decode(pdu_hex, pdu, sizeof(pdu), &pdu_len);
    if (result != SMS_PDU_DECODE_OK) {
        return result;
    }
    memset(out, 0, sizeof(*out));
    if (pdu_len < 2) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    size_t pos = 0;
    const size_t smsc_len = pdu[pos++];
    if (pos + smsc_len >= pdu_len) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    pos += smsc_len;
    const uint8_t first = pdu[pos++];
    if ((first & 0x03U) != 0x02U || pos + 3 > pdu_len) {
        return SMS_PDU_DECODE_UNSUPPORTED;
    }
    out->message_reference = pdu[pos++];
    const size_t ra_digits = pdu[pos++];
    const uint8_t ra_toa = pdu[pos++];
    const size_t ra_bytes = (ra_digits + 1U) / 2U;
    if (pos + ra_bytes + 15U > pdu_len ||
        !address_decode(pdu + pos, pdu_len - pos, ra_digits, ra_toa,
                        out->recipient, sizeof(out->recipient))) {
        return SMS_PDU_DECODE_INVALID;
    }
    pos += ra_bytes;
    if (!timestamp_decode(pdu + pos, pdu_len - pos, out->service_center_timestamp,
                          sizeof(out->service_center_timestamp))) {
        return SMS_PDU_DECODE_INVALID;
    }
    pos += 7;
    if (!timestamp_decode(pdu + pos, pdu_len - pos, out->discharge_timestamp,
                          sizeof(out->discharge_timestamp))) {
        return SMS_PDU_DECODE_INVALID;
    }
    pos += 7;
    if (pos >= pdu_len) {
        return SMS_PDU_DECODE_TRUNCATED;
    }
    out->status = pdu[pos];
    out->delivered = out->status < 0x20U;
    return SMS_PDU_DECODE_OK;
}

const char *sms_encoding_name(sms_encoding_t encoding)
{
    switch (encoding) {
    case SMS_ENCODING_GSM7:
        return "gsm7";
    case SMS_ENCODING_UCS2:
        return "ucs2";
    case SMS_ENCODING_8BIT:
        return "8bit";
    case SMS_ENCODING_UNKNOWN:
    default:
        return "unknown";
    }
}
