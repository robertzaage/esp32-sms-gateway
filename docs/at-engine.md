# AT command handling

The AT engine is the gateway's single application-level owner of the modem command channel. USB discovery may probe candidate interfaces during setup, but once an AT port is selected, normal modem and SMS work goes through this engine.

## Why commands are serialized

AT modems are line-oriented state machines. Concurrent writes can corrupt commands, and prompt commands are especially sensitive. The engine therefore queues requests and executes one transaction at a time.

For SMS submission the entire exchange stays under one lock of ownership:

```text
AT+CMGS=...
    -> wait for ">"
    -> send PDU and Ctrl-Z
    -> wait for +CMGS and final result
```

There is no API that gives control back to a caller while the modem is sitting at a prompt.

## Responses and unsolicited messages

The parser accepts arbitrary USB fragmentation and normalizes modem line endings. It recognizes the usual final results:

- `OK`
- `ERROR`
- `+CME ERROR: ...`
- `+CMS ERROR: ...`

Unsolicited result codes (URCs) such as registration changes and incoming-SMS notifications are routed to a separate queue. If a prefix is explicitly expected by the active command, the command response wins; this is necessary for queries such as `AT+CREG?`, where `+CREG:` can also arrive asynchronously.

Multi-line URCs such as `+CMT`, `+CDS` and `+CBM` keep their continuation state. A message body containing the literal text `OK` cannot finish an unrelated command by accident.

## Retry rules

Retries are opt-in. Routine read-only commands can choose a bounded retry policy, but SMS submission is not blindly retried after an ambiguous timeout. Once bytes may have reached the modem or network, a retry can duplicate a real-world message.

A USB disconnect or fatal transport error aborts the active transaction as a transport error. When the modem returns, the USB layer probes a fresh AT session and stale callbacks from the old handle are ignored.

## Bounds

Queues, parser lines and captured responses are all bounded. Overflow is reported through diagnostics and cannot turn into a false successful AT result.

The parser/protocol pieces are plain C and are exercised by host tests using fragmented and interleaved modem streams. The ESP-IDF CI build is the integration compile gate for the FreeRTOS and USB-facing parts.
