# Networking and REST API

The gateway joins Wi-Fi as a station and exposes a small REST management API on the local network.

The complete machine-readable contract is [api/openapi.yaml](../api/openapi.yaml). This page covers the parts an operator normally needs.

## Wi-Fi provisioning portal

The gateway opens a WPA2-protected SoftAP named `SMS-Gateway-XXXXXX` when:

- no Wi-Fi credentials are stored (first boot);
- the saved network has been unreachable for `CONFIG_GATEWAY_WIFI_PORTAL_FALLBACK_SECONDS` (default 180 s). The gateway keeps retrying the saved network and closes the portal as soon as it reconnects;
- the **MENU** button is held for `CONFIG_GATEWAY_DISPLAY_PORTAL_HOLD_SECONDS` (default 5 s). This portal closes after `CONFIG_GATEWAY_WIFI_MANUAL_PORTAL_SECONDS` (default 10 min).

The SoftAP password is generated once, stored in NVS and shown only on the display. It uses uppercase letters and digits without look-alike characters. A captive DNS responder sends every lookup to `192.168.4.1`, so most devices open the page by themselves.

The page offers a Wi-Fi scan, the Wi-Fi password, MQTT broker settings, Home Assistant discovery, a default notify recipient and the optional API token. Empty password and token fields keep the stored values. On submit, the gateway tests the new Wi-Fi credentials and reports `connecting`, `connected` or `failed` (with the 802.11 reason code) on the page and the display. On success it restarts so every service starts with the new settings.

Portal endpoints (only reachable on the SoftAP): `GET /api/setup/info`, `GET /api/setup/scan`, `POST /api/setup` and `GET /api/setup/status`. The REST management API and the portal share port 80. The API is stopped while the portal is open and restarts automatically afterwards.

Once connected, the gateway reconnects with capped backoff (1–30 s) and starts SNTP after it has an IPv4 address.

## API token

Set the token in the setup portal, or leave the field empty to have the gateway generate one. A generated token is shown once on the setup page and never logged. Only its SHA-256 digest is stored in NVS.

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

The API can submit a volatile SIM PIN, request modem recovery and reboot the gateway. Raw AT access is limited to the diagnostic endpoint below.

## Modem AT diagnostics

`POST /api/v1/modem/at` runs one AT command on the modem and returns its response lines, for hardware bring-up and troubleshooting:

```sh
curl -X POST http://sms-gateway.local/api/v1/modem/at \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"command":"AT+CPMS?","timeout_ms":10000}'
```

The command must start with `AT` and must not contain line breaks. `timeout_ms` is optional (100–60000, default 10000). `AT+CMGS` and `AT+CMGW` are refused: send messages through `/api/v1/messages`. Commands run on the same serialized channel as the gateway itself, so changing modem settings (for example `AT+CMGF` or `AT+CNMI`) can break SMS handling until the modem is restarted.

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
