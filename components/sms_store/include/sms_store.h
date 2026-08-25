#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sms_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_STORE_PARTITION "storage"
#define SMS_STORE_MAX_RECORDS 128
#define SMS_MESSAGE_TEXT_MAX 4096

typedef enum {
    SMS_DIRECTION_INBOUND = 0,
    SMS_DIRECTION_OUTBOUND,
} sms_direction_t;

typedef enum {
    SMS_MESSAGE_PARTIAL = 0,
    SMS_MESSAGE_RECEIVED,
    SMS_MESSAGE_QUEUED,
    SMS_MESSAGE_SENDING,
    SMS_MESSAGE_SENT,
    SMS_MESSAGE_DELIVERED,
    SMS_MESSAGE_FAILED,
    SMS_MESSAGE_UNSUPPORTED,
    SMS_MESSAGE_UNCERTAIN,
} sms_message_status_t;

typedef struct {
    uint32_t pruned_records;
    uint32_t capacity_failures;
    uint32_t replay_watermark;
    bool replay_protection_enabled;
    uint16_t used_records;
    uint16_t free_records;
} sms_store_diagnostics_t;

typedef struct {
    uint32_t id; /* Stable local identifier; zero requests allocation on save. */
    sms_direction_t direction;
    sms_message_status_t status;
    sms_encoding_t encoding;
    char sender[SMS_MAX_ADDRESS_LENGTH];
    char recipient[SMS_MAX_ADDRESS_LENGTH];
    char service_center_timestamp[40];
    char text[SMS_MESSAGE_TEXT_MAX];
    char raw_pdu[SMS_MAX_PDU_HEX]; /* Set for preserved unsupported/binary inbound SMS. */

    sms_concat_info_t concat;
    bool delivery_report_requested;
    uint16_t concat_reference;
    uint8_t segment_count;
    uint16_t segment_sent_mask;
    uint16_t segment_delivered_mask;
    uint16_t segment_failed_mask;
    uint8_t modem_reference[SMS_MAX_SEGMENTS];
    uint8_t delivery_status[SMS_MAX_SEGMENTS];
    uint8_t inflight_segment; /* 0 or one-based segment number. */
    uint8_t send_attempts;
    int last_modem_error;
} sms_message_t;

esp_err_t sms_store_init(void);

/** Save a new record or replace an existing record with the same non-zero id. */
esp_err_t sms_store_save(sms_message_t *message);
esp_err_t sms_store_get(uint32_t id, sms_message_t *out);
esp_err_t sms_store_delete(uint32_t id);

/** Find the oldest record with exactly this status. */
esp_err_t sms_store_find_status(sms_message_status_t status, sms_message_t *out);

/** Find a persisted inbound multipart segment. */
esp_err_t sms_store_find_partial(const char *sender,
                                 uint16_t reference,
                                 uint8_t total_parts,
                                 uint8_t part_number,
                                 sms_message_t *out);

/** Find a previously persisted complete inbound text message with the same network identity. */
esp_err_t sms_store_find_inbound_identity(const char *sender,
                                          const char *service_center_timestamp,
                                          sms_encoding_t encoding,
                                          const char *text,
                                          sms_message_t *out);

/** Find a previously quarantined unsupported inbound PDU. */
esp_err_t sms_store_find_inbound_raw_pdu(const char *pdu_hex, sms_message_t *out);

/** Find newest outbound message containing a modem TP-MR for this recipient. */
esp_err_t sms_store_find_outbound_reference(const char *recipient,
                                            uint8_t modem_reference,
                                            sms_message_t *out,
                                            size_t *segment_index);

/** List records with id > after_id in ascending id order. */
esp_err_t sms_store_list(uint32_t after_id,
                         sms_message_t *records,
                         size_t max_records,
                         size_t *record_count);

/** Configure the broker/event replay watermark used by conservative pruning.
 * When protection is enabled, inbound received records newer than watermark are
 * not eligible for automatic eviction. */
void sms_store_set_replay_watermark(bool protection_enabled, uint32_t watermark);
esp_err_t sms_store_get_diagnostics(sms_store_diagnostics_t *out);

const char *sms_message_status_name(sms_message_status_t status);

#ifdef __cplusplus
}
#endif
