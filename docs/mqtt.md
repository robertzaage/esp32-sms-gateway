# MQTT contract v1

MQTT is disabled by default. Configure it through `PATCH /api/v1/config/mqtt` and enable it only after a broker URI and topics validate.

Supported broker schemes are `mqtt://` and `mqtts://`. Credentials are configured separately from the URI. `mqtts://` verifies the broker with either a configured private CA PEM or Espressif's certificate bundle.

The default application root is:

```text
sms-gateway/<device_id>/
```

`device_id` is stable per gateway and contains no SIM, phone-number or modem identity.

## Topics

| Topic suffix | Direction | QoS | Retain | Purpose |
|---|---|---:|---:|---|
| `availability` | gateway -> broker | 1 | yes | `online`; LWT publishes `offline` |
| `status` | gateway -> broker | 1 | yes | non-sensitive modem/gateway status |
| `sms/send` | client -> gateway | 1 | no | durable structured SMS command |
| `command/result` | gateway -> broker | 1 | no | structured command acknowledgement |
| `sms/received` | gateway -> broker | 1 | no | incoming text-SMS event |
| `sms/status` | gateway -> broker | 1 | no | outbound/inbound durable status changes |
| `modem/restart` | client -> gateway | 1 | no | request controlled modem restart |
| `system/reboot` | client -> gateway | 1 | no | request gateway restart |
| `ha/notify` | Home Assistant -> gateway | 0 | no | native notify convenience path for configured default recipient |

SMS bodies/events are **never retained**. Only availability/status/discovery are retained.

## Structured SMS send

Publish to `<base>/sms/send` with QoS 1:

```json
{
  "request_id": "automation-unique-id-0001",
  "to": "+491701234567",
  "text": "Hello",
  "delivery_report": true
}
```

`request_id` is mandatory, 8-128 printable characters, and uses the same persistent idempotency ledger as REST. QoS-1 redelivery, reconnect, or ESP restart therefore cannot silently queue a second SMS with the same request. Reusing the ID with different content is rejected.

Acknowledgements are published on `<base>/command/result`, for example:

```json
{"request_id":"automation-unique-id-0001","ok":true,"code":"accepted","message_id":42}
```

Possible failure codes include `invalid_request`, `rate_limited`, `idempotency_conflict`, `idempotency_expired`, and `queue_failed`.

## Incoming SMS

`<base>/sms/received` carries non-retained JSON:

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

SMS originators may be numeric or alphanumeric. Treat message text as private data and configure broker retention/logging accordingly.

## Home Assistant

When enabled, firmware publishes one retained MQTT **device discovery** document to:

```text
<discovery_prefix>/device/<device_id>/config
```

The document includes required `device` and `origin` metadata and multiple components: modem/SIM/operator/signal/queue sensors, registration binary sensor, incoming-SMS event entity, restart buttons, and optionally a native MQTT notify entity.

The native notify entity uses `<base>/ha/notify` and exists only when `default_recipient` is configured. It is a convenience path at QoS 0 because Home Assistant's generic notify payload has no application-level idempotency key. For alarm/critical workflows use the structured `sms/send` topic and a unique `request_id` instead.

The gateway subscribes to `homeassistant/status`; an `online` event causes discovery/status to be republished.

Disabling Home Assistant discovery or changing broker/discovery configuration clears the old retained device-discovery payload before disconnecting when possible.

## Reliability and command safety

Application command topics are edge-triggered. The gateway rejects any command received with the MQTT retained flag set, so an old retained `sms/send`, `ha/notify`, `modem/restart`, or `system/reboot` message cannot execute when the gateway reconnects. Restart topics additionally require the exact payload `PRESS`.

Incoming text-SMS events use durable at-least-once delivery. The SMS record is already committed to the SMS journal before publication. Firmware publishes one inbound record at a time with QoS 1 and advances a persisted replay cursor only after `MQTT_EVENT_PUBLISHED` confirms the client-side acknowledgement flow. After broker/Wi-Fi/gateway interruption, records newer than that cursor are replayed in ID order. A lost PUBACK can therefore duplicate an MQTT/HA event, but cannot duplicate or lose the underlying SMS.

While MQTT is enabled, inbound SMS records newer than the replay cursor are protected from automatic storage-pressure pruning. Disabling MQTT removes that replay protection because no broker delivery is expected while the integration is disabled.
