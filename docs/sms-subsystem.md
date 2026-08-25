# SMS subsystem (M4)

M4 turns the managed AT channel into a durable text-SMS service. The service owns PDU encoding/decoding, receive processing, multipart reconstruction, outgoing segment progression, delivery reports and persistence. REST/MQTT layers consume this service later; they never issue SMS AT commands directly.

## Scope

Supported text encodings:

- GSM 03.38 / GSM 7-bit, including the extension table;
- UCS-2 DCS with UTF-16 surrogate-pair handling for modern Unicode characters;
- concatenated SMS using 8-bit or 16-bit concatenation references.

8-bit binary SMS is detected and preserved as a quarantined durable record containing the raw PDU, but is not exposed as text. This lets the firmware safely delete the modem copy and continue draining the inbox without pretending binary payloads are UTF-8. A later API extension may expose those raw records to privileged callers.

## Modem profile

Once `modem_manager` reaches `READY`, `sms_service` configures:

```text
AT+CMGF=0
AT+CNMI=2,1,0,1,0
```

PDU mode is mandatory internally. Text mode is not used as a fallback because it gives inconsistent Unicode/multipart behavior across modem firmware.

After each modem-ready transition, the service drains stored modem messages with `AT+CMGL=4`. The AT engine has a bounded response buffer, so the scan repeats after successfully persisted messages are deleted. This prevents a full modem inbox from being truncated to only the first response batch.

Fresh stored-message URCs are handled through both `+CMTI` and `+CDSI`. Direct `+CMT` and `+CDS` two-line URCs are also accepted.

## Receive durability

For a stored inbound SMS, the order is intentional:

```text
read PDU from modem
    -> decode/validate
    -> persist message or multipart part
    -> reconstruct multipart message when complete
    -> commit NVS
    -> delete modem/SIM index
    -> publish service event
```

If persistence fails, the modem copy is retained. A storage failure therefore creates backpressure instead of message loss.

Complete inbound text messages are deduplicated by sender, service-centre timestamp, encoding and decoded text. Multipart parts are additionally deduplicated by sender, concatenation reference, total part count and part number. This protects against repeated modem scans after an acknowledgement/delete failure without suppressing unrelated messages that happen to have the same body.

## Outbound durability and the `uncertain` state

An API/MQTT caller will eventually receive a local message ID only after the outbound record is committed to NVS. Sending then happens asynchronously.

Each segment follows:

```text
QUEUED
  -> persist SENDING + in-flight segment
  -> AT+CMGS=<tpdu-length>
  -> wait for '>'
  -> PDU hex + Ctrl-Z
  -> wait for +CMGS / final result
  -> persist network TP-MR and sent bit
```

Prompt transactions are never automatically retried. If power, USB or acknowledgement is lost after transmission may have begun, the message moves to `UNCERTAIN`. The firmware cannot safely infer whether the network accepted that segment, so automatic retry could duplicate an SMS.

On boot, any record left in `SENDING` is converted to `UNCERTAIN`. Retrying an uncertain message is an explicit administrative action and carries a documented duplicate-send risk.

A failure that occurs before any modem bytes can be sent, such as local payload-allocation failure, returns the message to `QUEUED` because retransmission is known to be safe.

## Delivery reports

When delivery reports are requested, the TP-SRR bit is set on every submitted segment. `SMS-STATUS-REPORT` PDUs are matched to the newest outbound record with the same recipient and modem TP-MR. Segment delivery/failure masks are committed after each report.

A multipart message becomes `DELIVERED` only when every segment is delivered. A permanent failure report moves the message to `FAILED`.

## Persistent storage

The `storage` partition is a dedicated NVS data partition. SMS records are not raw C-struct dumps: the on-flash record has an explicit magic, version and fixed-width field layout so future firmware can introduce controlled migrations rather than depend on compiler padding.

Current bounds:

- 128 records;
- up to 16 SMS segments per logical message;
- up to 4095 bytes of retained UTF-8 text per logical message.

The store never auto-erases the partition after an NVS initialization or record-format error. When the journal is full, new writes fail safely. Automatic retention/pruning is intentionally deferred until policy can distinguish messages an operator wants retained from terminal records that may be discarded.

## Memory model

Large message records and receive/assembly workspaces are allocated from the heap rather than nested on the SMS FreeRTOS task stack. The NVS serializer uses shared static work buffers protected by the store mutex. This prevents multipart receive paths from consuming several 4 KiB objects on a 12 KiB task stack.

## Privacy

Normal logs contain local message ID, direction, status and segment count only. They do not include SMS bodies, phone numbers, alphanumeric sender IDs or PDU payloads. Raw PDU logging must remain an explicitly enabled diagnostic mode if added later.
