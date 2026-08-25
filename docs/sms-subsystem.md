# SMS behavior

The gateway uses PDU mode internally so text encoding, multipart messages and delivery reports behave consistently across modem firmware.

## Supported text

Outgoing and incoming text SMS supports:

- GSM 7-bit, including the extension table;
- UCS-2/UTF-16 for Unicode text, including surrogate pairs;
- concatenated messages with 8-bit or 16-bit references.

A logical message is limited to 16 SMS segments and 4095 bytes of retained UTF-8 text. REST and structured MQTT sends are preflighted before they enter the durable queue.

Binary/unsupported text formats are not guessed. 8-bit payloads, compressed DCS and GSM national-language-shift messages that the codec cannot safely interpret are preserved as quarantined raw-PDU records instead of being exposed as incorrect text.

## Receiving messages

When the modem reports a stored SMS, the gateway follows this order:

```text
read from modem
    -> decode/validate
    -> persist message or multipart part
    -> commit flash state
    -> delete the modem copy
    -> publish the event
```

If persistence fails, the modem copy is left in place. Storage pressure should therefore cause backpressure rather than silent message loss.

The service handles stored-message notifications, direct incoming SMS/delivery-report URCs and an inbox scan after the modem becomes ready. Multipart parts can arrive across reboots; assembly state is durable.

## Sending messages

A caller receives a local message ID only after the outbound record is stored in flash. Actual cellular submission happens asynchronously.

Each segment is marked as in progress before `AT+CMGS` begins. If the modem confirms the submission, the network reference and segment state are committed.

## The `uncertain` state

SMS submission has an unavoidable ambiguity: USB, power or the final modem response can disappear after the modem/network accepted a message but before the gateway sees confirmation.

In that case the gateway marks the message `uncertain` and does **not** automatically retry. An automatic retry could send the same real-world SMS twice.

After a reboot, any record left mid-send is also treated as uncertain. Retrying requires an explicit operator action with duplicate-risk acknowledgement.

Failures that happen before transmission can begin safely return to the queue.

## Delivery reports

When requested, the gateway asks for a delivery report on each segment and correlates reports using recipient and modem message-reference information. A multipart message reaches `delivered` only when all segments have delivery confirmation. Permanent negative reports move the message to `failed`.

## Storage

SMS records live in a dedicated NVS partition using a versioned on-flash format rather than compiler-dependent raw C structs.

The journal holds up to 128 logical records. Under storage pressure it may prune the oldest records that are known to be safe to discard, such as delivered/failed terminal records and incoming records that have already crossed the MQTT replay watermark.

It will not automatically prune queued, sending, uncertain or partial messages, sent messages that are still awaiting requested delivery reports, or incoming records that MQTT still needs to publish. If every record is protected, new writes fail rather than deleting important state.

## Privacy

Normal logs use local IDs, status and segment counts. SMS text, phone numbers, sender IDs and raw PDUs should remain out of normal log output.
