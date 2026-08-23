# Home Assistant

Core integration is intentionally based on Home Assistant's built-in MQTT integration and MQTT Discovery. HACS is **not** required.

Firmware will discover status sensors/binary sensors/buttons, an incoming SMS event entity, and optional `notify` entities for configured recipient aliases. For arbitrary numbers, import/adapt `send_sms.yaml`, which publishes a structured request to the gateway's normal MQTT command topic.

A richer custom integration may be offered later for a dedicated `send_sms(number, message)` UX and inbox views, but it is not an architectural dependency.
