# MQTT

MQTT is optional and disabled until configured through `PATCH /api/v1/config/mqtt`.

The gateway supports `mqtt://` and `mqtts://`. Broker credentials are configured separately from the URI. With `mqtts://`, the broker certificate is verified with either a configured private CA or Espressif's certificate bundle.

The default topic root is:

```text
sms-gateway/<device_id>/
```

The device ID is stable for the gateway and does not contain the SIM number, phone number or IMEI.

## Topics

| Suffix | Direction | QoS | Retained | Purpose |
|---|---|---:|---:|---|
| `availability` | gateway → broker | 1 | yes | `online`; LWT publishes `offline` |
| `status` | gateway → broker | 1 | yes | non-sensitive gateway/modem status |
| `sms/send` | client → gateway | 1 | no | structured SMS command |
| `command/result` | gateway → broker | 1 | no | command result |
| `sms/received` | gateway → broker | 1 | no | incoming SMS event |
| `sms/status` | gateway → broker | 1 | no | durable message status change |
| `modem/restart` | client → gateway | 1 | no | restart/recover modem |
| `system/reboot` | client → gateway | 1 | no | reboot gateway |
| `ha/notify` | Home Assistant → gateway | 0 | no | convenience send to the configured default recipient |

SMS bodies and SMS events are never retained by the gateway.

## Send an SMS

Publish JSON to `<base>/sms/send` with QoS 1:

```json
{
  "request_id": "automation-unique-id-0001",
  "to": "+491701234567",
  "text": "Hello",
  "delivery_report": true
}
```

`request_id` is required and is stored in the same persistent idempotency ledger used by REST. Re-delivery after reconnect or reboot therefore does not silently queue another SMS with the same request. Reusing the ID with different content is rejected.

Results arrive on `<base>/command/result`:

```json
{"request_id":"automation-unique-id-0001","ok":true,"code":"accepted","message_id":42}
```

Invalid input, rate limiting, idempotency conflicts, messages that exceed the 16-segment limit and queue failures are returned as structured failure results.

## Receive SMS

`<base>/sms/received` contains a non-retained event such as:

```json
{
  "id": 41,
  "status": "received",
  "event_type": "received",
  "from": "BANK",
  "text": "Example message",
  "received_at": "2026-08-24T08:31:12+02:00"
}
```

An originator can be a phone number or an alphanumeric sender ID.

Incoming MQTT delivery is **at least once**. The SMS is committed to flash before publication, and the gateway advances a persistent replay cursor only after the MQTT publish is acknowledged. A lost acknowledgement can repeat an event after reconnect, so consumers should use the durable message `id` when deduplication matters.

While MQTT is enabled, incoming SMS records that have not crossed that replay cursor are protected from storage-pressure pruning.

## Command safety

The gateway rejects retained messages on all command topics. This prevents an old retained send/restart command from executing when the gateway reconnects.

`modem/restart` and `system/reboot` additionally require the exact payload:

```text
PRESS
```

Structured `sms/send` commands use `request_id` for application-level duplicate protection. Control commands do not have the same idempotency key, so do not intentionally publish them repeatedly with QoS 1.

## Home Assistant

When Home Assistant discovery is enabled, the gateway publishes a retained device discovery document under:

```text
<discovery_prefix>/device/<device_id>/config
```

It creates gateway/modem diagnostics, registration state, an incoming-SMS event, restart buttons and—when a default recipient is configured—an MQTT notify entity.

The notify entity is a convenience path and uses QoS 0 because Home Assistant's generic notify payload does not provide an application idempotency key. For alarms or arbitrary recipients, use the structured `sms/send` topic with a unique `request_id`.

See [Home Assistant](../homeassistant/README.md) for examples.
