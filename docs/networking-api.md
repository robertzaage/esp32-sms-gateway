# Networking and REST API

The gateway joins Wi-Fi as a station and exposes a small REST management API on the local network.

The complete machine-readable contract is [api/openapi.yaml](../api/openapi.yaml). This page covers the parts an operator normally needs.

## Wi-Fi provisioning

A fresh device starts Espressif SoftAP provisioning. The serial console prints the provisioning service name and a temporary proof-of-possession value. Wi-Fi credentials are handled by ESP-IDF and are not printed after provisioning succeeds.

Once connected, the gateway uses capped reconnect backoff and starts SNTP after it has an IPv4 address.

## API token

On first boot the gateway creates a random 256-bit bearer token and prints it once:

```text
INITIAL_API_TOKEN=...
```

Only a SHA-256 digest of that token is stored in NVS. Save the plaintext token somewhere appropriate for your deployment.

`/api/v1/health` is unauthenticated. Other `/api/v1/*` routes require:

```text
Authorization: Bearer <token>
```

The management listener is plain HTTP. Keep it on a trusted LAN or access it through a trusted VPN/TLS reverse proxy.

## Check status

```sh
curl http://sms-gateway.local/api/v1/health

curl \
  -H "Authorization: Bearer $TOKEN" \
  http://sms-gateway.local/api/v1/status
```

`/status` includes network, modem/SIM, signal, SMS journal pressure, USB recovery/over-current, idempotency, MQTT replay and OTA state.

## Send SMS

```sh
curl --fail-with-body \
  -X POST http://sms-gateway.local/api/v1/messages \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: invoice-alert-00042' \
  -d '{"to":"+491701234567","text":"Invoice 42 is ready","delivery_report":true}'
```

A successful request returns `202 Accepted` after the outbound record is committed to flash. It does not wait for cellular delivery.

Use a unique `Idempotency-Key` for retried client requests. Repeating the same key and same SMS returns the existing durable message rather than queuing another one. Reusing a key for different content is rejected.

Messages are preflighted before they enter the queue. Text that cannot be represented within the firmware's 16-segment GSM-7/UCS-2 limit returns HTTP `422`.

## Uncertain sends

If power or USB is lost at a point where the modem may already have accepted a segment, the message becomes `uncertain`. It is not automatically retried.

An operator can retry through `/api/v1/messages/{id}/retry` only by sending:

```json
{"acknowledge_duplicate_risk": true}
```

That explicit acknowledgement exists because the retry can produce a duplicate SMS.

## SIM and recovery

The API can submit a volatile SIM PIN, request modem recovery and reboot the gateway. There is intentionally no general-purpose raw AT endpoint.

## MQTT configuration

`GET /api/v1/config/mqtt` returns redacted settings and runtime state. `PATCH /api/v1/config/mqtt` updates broker, TLS and Home Assistant settings. Passwords and private CA contents are write-only; reads expose only whether those values are configured.

See [MQTT](mqtt.md) for the topic contract.

## Idempotency recovery

The gateway uses two-phase persistent reservations to make REST/MQTT retry behavior conservative across resets. A reservation whose final message ID could not be recorded is not silently expired, because doing so might permit a duplicate SMS.

If an operator decides to clear stranded reservations, use `POST /api/v1/system/idempotency/clear-pending` with:

```json
{"acknowledge_duplicate_risk": true}
```

This is an emergency recovery action, not routine maintenance.

## Firmware updates

`GET /api/v1/system/firmware` reports the running and selected boot image plus rollback/OTA state.

`POST /api/v1/system/firmware` accepts the raw `*-ota.bin` application image as `application/octet-stream` and requires an `X-Firmware-SHA256` header. See [OTA and releases](ota-releases.md) for the complete example and version policy.
