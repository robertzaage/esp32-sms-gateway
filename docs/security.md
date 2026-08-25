# Security and privacy

The gateway handles data that is easy to leak accidentally: SMS content, phone numbers, SIM credentials, Wi-Fi credentials and management tokens. The default design tries to keep those values out of normal logs and status channels, but deployment choices still matter.

## REST API

The REST management listener is plain HTTP. Use it only on a trusted management LAN or through a trusted VPN/TLS reverse proxy. Do not forward the API or OTA endpoint directly to the Internet.

On first boot the gateway generates a random bearer token and prints it once to the serial console. Only the token's SHA-256 digest is kept in NVS. Protect the plaintext token like a password.

There is no arbitrary raw-AT REST or MQTT endpoint.

## Wi-Fi provisioning

Initial Wi-Fi onboarding uses Espressif provisioning with a temporary random proof-of-possession value. Wi-Fi credentials are stored by the normal ESP-IDF Wi-Fi/NVS subsystem and should not appear in normal application logs.

## MQTT

Use `mqtts://` when the broker is not on a trusted network. TLS verifies the broker using either a configured private CA or Espressif's certificate bundle.

MQTT passwords and custom CA content are write-only through the management API and are not returned by configuration reads. The service keeps private CA memory alive for the full MQTT client lifetime because ESP-MQTT retains a pointer to that data.

All MQTT command topics reject retained messages. Restart/reboot commands also require the exact payload `PRESS`.

Incoming SMS events are at-least-once, so consumers may see the same event again after acknowledgement loss. Use the durable message ID to deduplicate if needed.

## SMS and SIM data

Normal logs should not contain SMS bodies, recipients, sender IDs, raw PDUs, SIM PINs or IMEI values.

SIM PIN submission is volatile and temporary command buffers are wiped after use.

SMS records are intentionally durable in flash. If physical access to the device is part of your threat model, use the ESP32-S3 flash-encryption and Secure Boot features appropriate to the deployment.

Home Assistant and other MQTT consumers can persist incoming SMS content in their own databases or logs. Configure Recorder, broker logging and downstream automations according to your privacy requirements.

## Idempotency and duplicate risk

Persistent request idempotency prevents normal REST/MQTT retries from queuing duplicate SMS. The gateway deliberately does not auto-expire an idempotency reservation whose final outcome is unknown.

Clearing stranded reservations or retrying an `uncertain` SMS requires an explicit acknowledgement of duplicate-send risk. Those endpoints are recovery tools, not routine workflow APIs.

## OTA

The OTA endpoint requires bearer authentication and the SHA-256 of the exact application image. The firmware also checks project/version policy and relies on ESP-IDF image validation before selecting the new boot partition.

SHA-256 over plain HTTP is an integrity check, not image authentication. An active attacker on the management network could replace both the image and digest header. Use a trusted network/VPN/TLS proxy, or deploy signed-image/Secure Boot controls for stronger authenticity guarantees.

A new OTA image is confirmed only after critical services start and the stability window passes. Resetting before confirmation leaves ESP-IDF free to roll back to the previous valid image.
