# AT engine

M2 introduces the transport-independent AT transaction engine used by the modem manager and SMS service.

## Ownership and concurrency

The AT engine is the **only application-layer writer** to the selected modem transport after USB probing. Callers may invoke `at_engine_execute()` concurrently; requests are copied into a bounded FreeRTOS queue and executed serially by `at_worker`.

The USB receive callback never parses responses and never blocks. It copies received bytes into bounded 256-byte queue chunks using a zero-timeout enqueue. The worker owns the incremental tokenizer and transaction state.

Unsolicited result codes (URCs) are copied into a separate bounded queue and delivered by the `at_urc` task. This prevents URC consumers from blocking the parser/transaction worker.

## Request model

A normal request supplies a command **without** the trailing carriage return:

```c
static const char *const expected[] = { "+CSQ:" };

at_request_t request = {
    .command = "AT+CSQ",
    .expected_prefixes = expected,
    .expected_prefix_count = 1,
    .timeout_ms = 3000,
};

at_response_t response;
ESP_ERROR_CHECK(modem_core_at_execute(&request, &response));
```

The transport write is `AT+CSQ\r`. A successful response captures non-final response lines in bounded storage and completes on `OK`.

`at_engine_execute()` returning `ESP_OK` means the transaction was executed by the engine. The modem outcome is in `response.result` (`AT_RESULT_OK`, `AT_RESULT_CME_ERROR`, `AT_RESULT_TIMEOUT`, etc.).

## Prompt transactions

Prompt operations are atomic. `wait_for_prompt=true` requires a payload in the same request. The worker holds exclusive modem ownership across all three phases:

```text
AT+CMGS=...
      |
      v
wait for ">"
      |
      v
write exact payload (including Ctrl-Z when required)
      |
      v
wait for +CMGS / final OK or error
```

There is intentionally no API that returns modem ownership to the caller while the modem is sitting at a `>` prompt. That would allow another queued AT command to become SMS body data.

## Tokenizer

`at_parser` is pure C and host-testable. It handles arbitrary USB fragmentation, CR/LF combinations, a bounded 512-byte line, recovery after overlong lines, and prompt recognition only while the active transaction explicitly enables prompt mode.

Prompt mode is context-sensitive so an incoming SMS body beginning with `>` is not mistaken for a modem prompt.

## Response and URC routing

Known URC prefixes include registration updates, SMS notifications, USSD, ring/call indications, and network events. A prefix explicitly expected by the active command wins over URC classification. This handles commands such as `AT+CREG?`, where `+CREG:` is both a solicited response prefix and an unsolicited notification prefix.

Multi-line URCs are stateful:

- `+CMT:` consumes the following line as SMS payload/PDU.
- `+CDS:` consumes the following line as delivery-report payload/PDU.
- `+CBM:` consumes the following line as cell-broadcast payload.

Continuation routing occurs **before** final-result detection. Therefore an unsolicited SMS whose body is literally `OK` or `ERROR` cannot accidentally complete an unrelated AT command.

## Final results

M2 recognizes:

- `OK`
- `ERROR`
- `+CME ERROR: <code or text>`
- `+CMS ERROR: <code or text>`

Numeric CME/CMS codes are exposed in `response.error_code`; textual errors use `-1` and remain distinguishable by result type.

## Retry policy

Retries are opt-in and per request. The default is one attempt with no retry.

Available retry reasons are timeout, generic `ERROR`, CME error, CMS error, and transport error. A caller also chooses `max_attempts` and `retry_delay_ms`.

Only commands known to be idempotent should be automatically retried. In particular, SMS submission should **not** blindly retry after an ambiguous timeout because the network/modem may already have accepted the message.

For timeout retries, use a non-zero retry delay. The worker drains late complete lines before the next attempt to reduce the risk of a delayed final result being associated with the retry.

## Cancellation and disconnects

`at_engine_cancel_current()` aborts only the active transaction. Queued requests remain queued.

A USB disconnect calls `at_engine_notify_transport_lost()`. The active transaction wakes and completes with `AT_RESULT_TRANSPORT_ERROR`; future requests fail as transport-unavailable until USB probing establishes a new AT-ready interface.

## Bounds and diagnostics

Current bounded resources:

- request queue: 8 transactions
- RX queue: 20 x 256-byte chunks
- URC queue: 16 x 512-byte lines
- parser line: 512 bytes
- captured response: 2048 bytes / 24 lines

Queue or parser overflow is never silently treated as a successful command. Diagnostics count submissions, completions, retries, timeouts, cancellations, transport errors, RX/parser overflow, and URC delivery/drop events.

## Host tests

The pure-C tests deliberately fragment a representative modem stream using nearly 2,000 deterministic chunk patterns and verify identical tokens every time. They also test:

- CR/LF normalization
- prompt-mode isolation
- overlong-line recovery
- `OK`, `ERROR`, CME and CMS finals
- response-vs-URC prefix precedence
- interleaved multi-line URCs during an active command
- URC payloads that themselves equal `OK`

The ESP-IDF build remains the integration compile gate because FreeRTOS scheduling and the physical USB transport are target-side concerns.
