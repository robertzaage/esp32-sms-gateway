# MQTT contract (draft v1)

All application topics live below:

```text
sms-gateway/<device_id>/
```

`device_id` is stable per gateway and must not contain secrets or a phone number.

## Topics

| Topic suffix | Direction | QoS | Retain | Purpose |
|---|---|---:|---|---|
| `availability` | gateway -> broker | 1 | yes | `online` / LWT `offline` |
| `state` | gateway -> broker | 1 | yes | compact gateway state snapshot |
| `modem` | gateway -> broker | 1 | yes | operator/registration/signal state |
| `command/sms/send` | client -> gateway | 1 | no | queue outbound SMS |
| `event/sms/status` | gateway -> broker | 1 | no | queued/sending/sent/delivered/failed |
| `event/sms/received` | gateway -> broker | 1 | no | incoming SMS event |
| `event/system` | gateway -> broker | 1 | no | recovery/errors/restarts |

SMS event topics are never retained to avoid turning the broker retained-message store into an inbox.

## Send request

```json
{
  "id": "optional-client-idempotency-id",
  "to": "+491701234567",
  "text": "Hello",
  "delivery_report": true
}
```

The gateway validates E.164 recipients, persists accepted messages, and publishes status using the durable gateway message ID. A repeated client idempotency ID must not create a second SMS.

## Incoming event

```json
{
  "id": "01J...",
  "from": "+491701234567",
  "received_at": "2026-08-23T11:42:31+02:00",
  "text": "Hello",
  "encoding": "gsm7",
  "parts": 1
}
```

## Home Assistant

Firmware will publish native MQTT Discovery configs below Home Assistant's configured discovery prefix (normally `homeassistant`). Core support does not require HACS.

Planned entities:

- gateway availability
- modem state/operator/signal/registration
- queue depth and last error
- incoming SMS event entity
- restart modem/gateway buttons
- `notify` entities for configured recipient aliases

Arbitrary phone numbers remain available through `command/sms/send`; the repository ships a Home Assistant script example for this case.
