# Networking and REST API

The gateway joins Wi-Fi as a station and exposes a small REST management API on the local network.

The complete machine-readable contract is [api/openapi.yaml](../api/openapi.yaml). This page covers the parts an operator normally needs.

## Wi-Fi provisioning portal

A fresh device starts a WPA2-protected SoftAP. Its unique SSID and randomly generated password are shown on the board's LCD; no setup secret is printed to the serial console. Join the access point and open `http://192.168.4.1`. The captive DNS responder makes common operating-system captive-portal checks land on the same page.

The page accepts a home Wi-Fi SSID/password and a required 32–128 character API token. Credentials are saved through the ESP-IDF Wi-Fi driver and the token is immediately hashed with SHA-256 before it is committed to NVS. The portal, DNS responder and SoftAP stop before the gateway reconnects as a station.

Once connected, the gateway uses capped reconnect backoff and starts SNTP after it has an IPv4 address.

## API token

Create and save the token in the setup portal. It is not logged or displayed after the form is submitted. Only its SHA-256 digest is stored in NVS.

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
