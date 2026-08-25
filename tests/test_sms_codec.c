#include "sms_codec.h"
#include "sms_gsm7.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_gsm7_alphabet(void)
{
    const char *text = "Hello []{}\\^~| euro=€ £ Δ";
    uint8_t septets[128];
    size_t count = 0;
    assert(sms_gsm7_encode(text, septets, sizeof(septets), &count));
    assert(count == sms_gsm7_septet_count(text));
    char decoded[256];
    assert(sms_gsm7_decode(septets, count, decoded, sizeof(decoded)));
    assert(strcmp(text, decoded) == 0);

    assert(sms_gsm7_septet_count("🙂") == SIZE_MAX);
}

static void test_submit_hello(void)
{
    sms_submit_segment_t segments[2] = {0};
    size_t count = 0;
    assert(sms_submit_encode("+1989450681", "Hello", false, 0x42,
                             segments, 2, &count));
    assert(count == 1);
    assert(segments[0].encoding == SMS_ENCODING_GSM7);
    assert(segments[0].tpdu_length_octets == 17);
    /* Microsoft example differs only in first-octet validity-period policy and SMSC. */
    assert(strstr(segments[0].pdu_hex, "C8329BFD06") != NULL);
}

static void test_submit_multipart_gsm7(void)
{
    char text[220];
    memset(text, 'A', 200);
    text[200] = '\0';
    sms_submit_segment_t segments[SMS_MAX_SEGMENTS] = {0};
    size_t count = 0;
    assert(sms_submit_encode("+491701234567", text, true, 0x44,
                             segments, SMS_MAX_SEGMENTS, &count));
    assert(count == 2);
    assert(segments[0].concat.present);
    assert(!segments[0].concat.is_16bit);
    assert(segments[0].concat.reference == 0x44);
    assert(segments[0].concat.total_parts == 2);
    assert(segments[0].concat.part_number == 1);
    assert(segments[1].concat.part_number == 2);
    /* Empty SMSC, TP-SUBMIT + SRR + UDHI. */
    assert(strncmp(segments[0].pdu_hex, "0061", 4) == 0);
}

static void test_submit_ucs2_and_surrogate_pair(void)
{
    sms_submit_segment_t segments[SMS_MAX_SEGMENTS] = {0};
    size_t count = 0;
    assert(sms_submit_encode("+491701234567", "Grüße 🙂", false, 0x1234,
                             segments, SMS_MAX_SEGMENTS, &count));
    assert(count == 1);
    assert(segments[0].encoding == SMS_ENCODING_UCS2);
    /* UTF-16BE surrogate pair for U+1F642. */
    assert(strstr(segments[0].pdu_hex, "D83DDE42") != NULL);

    char long_text[1024] = {0};
    size_t pos = 0;
    for (int i = 0; i < 80; ++i) {
        const char *emoji = "🙂";
        memcpy(long_text + pos, emoji, strlen(emoji));
        pos += strlen(emoji);
    }
    long_text[pos] = '\0';
    assert(sms_submit_encode("+491701234567", long_text, false, 0x1234,
                             segments, SMS_MAX_SEGMENTS, &count));
    assert(count >= 3);
    for (size_t i = 0; i < count; ++i) {
        assert(segments[i].encoding == SMS_ENCODING_UCS2);
        assert(segments[i].concat.present);
        assert(segments[i].concat.is_16bit);
        assert(segments[i].concat.reference == 0x1234);
    }
}

static void test_deliver_known_gsm7(void)
{
    sms_deliver_t sms;
    /* Public GSM PDU example: sender +46705930301, body "Test". */
    assert(sms_deliver_decode("07916407058099F9040B916407950303F100008921222140140004D4E2940A", &sms)
           == SMS_PDU_DECODE_OK);
    assert(strcmp(sms.sender, "+46705930301") == 0);
    assert(strcmp(sms.text, "TEST") == 0);
    assert(sms.encoding == SMS_ENCODING_GSM7);
    assert(!sms.concat.present);
}

static void test_deliver_hellohello(void)
{
    sms_deliver_t sms;
    assert(sms_deliver_decode("07917238010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37", &sms)
           == SMS_PDU_DECODE_OK);
    assert(strcmp(sms.text, "hellohello") == 0);
    assert(strcmp(sms.service_center_timestamp, "1999-03-29T15:16:59+02:00") == 0);
}


static void test_deliver_alphanumeric_sender(void)
{
    sms_deliver_t sms;
    assert(sms_deliver_decode("0791448720003023240DD0E474D81C0EBB010000111011315214000BE474D81C0EBB5DE3771B", &sms)
           == SMS_PDU_DECODE_OK);
    assert(strcmp(sms.sender, "diafaan") == 0);
    assert(strcmp(sms.text, "diafaan.com") == 0);
}

static void test_deliver_multipart_ucs2(void)
{
    sms_deliver_t sms;
    assert(sms_deliver_decode("00400B911605935713F20008814080611373238C050003C00301007400680069007300200069007300200061002000760065007200790020006C006F006E00670020006D0065007300730061006700650020007400680061007400200064006F006500730020006E006F0074002000660069007400200069006E00200061002000730069006E0067006C006500200053004D00530020006D0065007300730061", &sms)
           == SMS_PDU_DECODE_OK);
    assert(sms.encoding == SMS_ENCODING_UCS2);
    assert(sms.concat.present);
    assert(sms.concat.reference == 0xC0);
    assert(sms.concat.total_parts == 3);
    assert(sms.concat.part_number == 1);
    assert(strncmp(sms.text, "this is a very long message", 27) == 0);
}

static void test_deliver_compressed_is_unsupported(void)
{
    sms_deliver_t sms;
    /* Same public TEST vector, but DCS compression bit is set (0x20). */
    assert(sms_deliver_decode("07916407058099F9040B916407950303F100208921222140140004D4E2940A", &sms)
           == SMS_PDU_DECODE_UNSUPPORTED);
    assert(sms.encoding == SMS_ENCODING_UNKNOWN);
}

static void test_deliver_binary_is_preservable_unsupported(void)
{
    sms_deliver_t sms;
    const char *pdu = "07916407058099F9040B916407950303F100048921222140140004DEADBEEF";
    assert(sms_pdu_type_detect(pdu) == SMS_PDU_TYPE_DELIVER);
    assert(sms_deliver_decode(pdu, &sms) == SMS_PDU_DECODE_UNSUPPORTED);
    assert(sms.encoding == SMS_ENCODING_8BIT);
    assert(strcmp(sms.sender, "+46705930301") == 0);
}

static void test_status_report(void)
{
    sms_status_report_t report;
    assert(sms_status_report_decode("0006D60B911326880736F4111011719551401110117195714000", &report)
           == SMS_PDU_DECODE_OK);
    assert(report.message_reference == 0xD6);
    assert(strcmp(report.recipient, "+31628870634") == 0);
    assert(report.status == 0x00);
    assert(report.delivered);
}

static void test_invalid(void)
{
    sms_deliver_t sms;
    assert(sms_deliver_decode("0", &sms) == SMS_PDU_DECODE_INVALID_HEX);
    assert(sms_deliver_decode("00GG", &sms) == SMS_PDU_DECODE_INVALID_HEX);

    sms_submit_segment_t segment;
    size_t count = 0;
    assert(!sms_submit_encode("+49-170", "hello", false, 1, &segment, 1, &count));
}

int main(void)
{
    test_gsm7_alphabet();
    test_submit_hello();
    test_submit_multipart_gsm7();
    test_submit_ucs2_and_surrogate_pair();
    test_deliver_known_gsm7();
    test_deliver_hellohello();
    test_deliver_alphanumeric_sender();
    test_deliver_multipart_ucs2();
    test_deliver_compressed_is_unsupported();
    test_deliver_binary_is_preservable_unsupported();
    test_status_report();
    test_invalid();

    sms_encoding_t pf_encoding = SMS_ENCODING_UNKNOWN;
    size_t pf_segments = 0;
    assert(sms_submit_preflight("+4912345", "hello", 0x1234, &pf_encoding, &pf_segments));
    assert(pf_encoding == SMS_ENCODING_GSM7 && pf_segments == 1);
    char too_long[3000]; memset(too_long, 'A', sizeof(too_long) - 1U); too_long[sizeof(too_long)-1U] = '\0';
    assert(!sms_submit_preflight("+4912345", too_long, 0x1234, &pf_encoding, &pf_segments));
    puts("sms codec tests passed");
    return 0;
}
