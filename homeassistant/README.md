# Home Assistant

The gateway works with Home Assistant's built-in MQTT integration. No HACS component or custom integration is required.

Enable MQTT and Home Assistant discovery in the gateway configuration. The gateway publishes one MQTT device with useful diagnostics, network registration, an incoming-SMS event, restart buttons and an optional notify entity.

## Receive SMS

Incoming messages appear through the discovered event entity. Event data includes the sender, text, durable message ID and service-center timestamp.

SMS content is private. Home Assistant Recorder, automation traces or broker logs may keep event attributes after the gateway has published them. Exclude the entity or design your automation accordingly if you do not want SMS content stored in Home Assistant.

## Send to one default number

If `default_recipient` is configured in the gateway's MQTT settings, discovery creates a native MQTT notify entity. This is convenient for ordinary notifications to one number.

The notify path uses QoS 0 because Home Assistant's generic notify command does not supply an application idempotency key. It is not the preferred path for critical alerts where duplicate/lost-command behavior matters.

## Send to arbitrary recipients

Use the example in `send_sms.yaml` or publish the structured MQTT command yourself. Structured sends include a unique `request_id`, which the gateway stores in its persistent idempotency ledger.

A command looks like:

```json
{
  "request_id": "ha-alarm-20260825-001",
  "to": "+491701234567",
  "text": "Alarm triggered",
  "delivery_report": true
}
```

Publish it to:

```text
sms-gateway/<device_id>/sms/send
```

with QoS 1.

For an automation, generate a request ID that is stable for one logical alert but different for the next alert. That lets broker reconnect/redelivery repeat the same command safely without turning it into another SMS.

## Restart buttons

The discovered modem and gateway restart buttons publish the exact `PRESS` payload expected by the firmware. Retained command messages are rejected, so an old restart request cannot execute merely because the gateway reconnects to MQTT.

For the full topic and reliability contract, see [docs/mqtt.md](../docs/mqtt.md).
