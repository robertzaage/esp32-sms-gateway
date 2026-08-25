# Home Assistant integration

No HACS component is required. Enable Home Assistant support in the gateway MQTT configuration and use Home Assistant's built-in MQTT integration.

The gateway publishes MQTT Device Discovery for one `SMS Gateway` device containing modem/SIM/operator/signal/queue diagnostics, network registration, an incoming-SMS event entity, restart buttons, and an optional native `notify` entity when a default recipient is configured.

## Default-recipient notify entity

Configure `default_recipient` as an E.164 number in `/api/v1/config/mqtt`. After discovery, Home Assistant exposes an MQTT notify entity. It is intended for convenient non-critical notifications.

## Arbitrary recipients / critical automations

Use `send_sms.yaml`. It publishes to the structured QoS-1 command topic with an application `request_id`, allowing the gateway's persistent idempotency ledger to suppress broker redeliveries after reconnect/reboot.

## Receiving SMS

The discovered event entity receives JSON from `<base>/sms/received`. The `event_type` is `received`; sender, text, durable message ID and service-center timestamp are event attributes.

SMS content is private. Home Assistant automations and Recorder may persist event attributes depending on your configuration. If message content should not enter the Recorder database, exclude the relevant entity or process the raw MQTT topic in an automation designed for your privacy requirements.
