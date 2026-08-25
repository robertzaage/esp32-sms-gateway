#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_MAX_ADDRESS_LENGTH 24
#define SMS_MAX_TEXT_UTF8 2048
#define SMS_MAX_PDU_OCTETS 176
#define SMS_MAX_PDU_HEX ((SMS_MAX_PDU_OCTETS * 2) + 1)
#define SMS_MAX_SEGMENTS 16

typedef enum {
    SMS_ENCODING_GSM7 = 0,
    SMS_ENCODING_UCS2,
    SMS_ENCODING_8BIT,
    SMS_ENCODING_UNKNOWN,
} sms_encoding_t;

typedef enum {
    SMS_PDU_TYPE_UNKNOWN = 0,
    SMS_PDU_TYPE_DELIVER,
    SMS_PDU_TYPE_STATUS_REPORT,
} sms_pdu_type_t;

typedef enum {
    SMS_PDU_DECODE_OK = 0,
    SMS_PDU_DECODE_INVALID_HEX,
    SMS_PDU_DECODE_TRUNCATED,
    SMS_PDU_DECODE_UNSUPPORTED,
    SMS_PDU_DECODE_INVALID,
    SMS_PDU_DECODE_TEXT_TOO_LONG,
} sms_pdu_decode_result_t;

typedef struct {
    bool present;
    bool is_16bit;
    uint16_t reference;
    uint8_t total_parts;
    uint8_t part_number;
} sms_concat_info_t;

typedef struct {
    sms_encoding_t encoding;
    sms_concat_info_t concat;
    uint8_t segment_number;
    uint8_t segment_count;
    size_t tpdu_length_octets;
    char pdu_hex[SMS_MAX_PDU_HEX];
} sms_submit_segment_t;

typedef struct {
    char sender[SMS_MAX_ADDRESS_LENGTH];
    sms_encoding_t encoding;
    sms_concat_info_t concat;
    char service_center_timestamp[40];
    char text[SMS_MAX_TEXT_UTF8];
} sms_deliver_t;

typedef struct {
    uint8_t message_reference;
    char recipient[SMS_MAX_ADDRESS_LENGTH];
    char service_center_timestamp[40];
    char discharge_timestamp[40];
    uint8_t status;
    bool delivered;
} sms_status_report_t;

/**
 * Build one or more SMS-SUBMIT PDUs. SMSC is left empty so the modem/network
 * uses the configured service center. GSM 03.38 is preferred whenever all
 * input characters can be represented; otherwise UCS-2 is used.
 *
 * concat_reference is used only for multipart messages. request_status_report
 * sets TP-SRR. Returns false for invalid E.164-like addresses, unrepresentable
 * UCS-2 characters, excessive message length, or insufficient output slots.
 */
bool sms_submit_encode(const char *recipient,
                       const char *utf8_text,
                       bool request_status_report,
                       uint16_t concat_reference,
                       sms_submit_segment_t *segments,
                       size_t max_segments,
                       size_t *segment_count);

/** Validate an outbound message without constructing PDUs. Uses the supplied
 * concatenation reference to account for 8-bit vs 16-bit UDH capacity. */
bool sms_submit_preflight(const char *recipient,
                          const char *utf8_text,
                          uint16_t concat_reference,
                          sms_encoding_t *encoding,
                          size_t *segment_count);

sms_pdu_type_t sms_pdu_type_detect(const char *pdu_hex);
sms_pdu_decode_result_t sms_deliver_decode(const char *pdu_hex, sms_deliver_t *out);
sms_pdu_decode_result_t sms_status_report_decode(const char *pdu_hex, sms_status_report_t *out);

const char *sms_encoding_name(sms_encoding_t encoding);

#ifdef __cplusplus
}
#endif
