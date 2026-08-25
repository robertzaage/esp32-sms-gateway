# Networking and REST API

M5 adds Wi-Fi onboarding and the first implemented HTTP management API.

## Wi-Fi provisioning

On an unprovisioned device the gateway starts Espressif `network_provisioning`
1.2.4 using the SoftAP scheme and protocomm Security 1 (X25519 + AES-CTR with
proof of possession). ESP-IDF 6 disables Security 1 unless explicitly enabled,
so `sdkconfig.defaults` turns it on deliberately.

The serial console prints a transient line containing the provisioning service
name and one-time PoP. Neither Wi-Fi credentials nor PoP are logged after
provisioning succeeds. Provisioned credentials remain in the normal Wi-Fi NVS
storage managed by ESP-IDF.

When provisioned, the gateway runs station mode with capped exponential
reconnect backoff. SNTP starts after an IPv4 address is obtained. A stable
hostname/device ID is derived from the station MAC suffix without publishing
the complete MAC address in normal application state.

## API authentication

On first boot the gateway creates 32 random bytes and renders them as a
64-character hexadecimal bearer token. Only SHA-256(token) is stored in NVS;
the plaintext token is printed to the serial console exactly once as
`INITIAL_API_TOKEN=...`. Losing that token currently requires erasing the normal
NVS partition and reprovisioning.

All `/api/v1/*` routes except `/api/v1/health` require:

```text
Authorization: Bearer <token>
```

Validation hashes the presented token and uses a constant-time digest compare.
Request bodies are bounded before allocation and parsed as JSON only after
authentication and global rate-limit checks.

## SMS safety

`POST /api/v1/messages` returns `202 Accepted` after the SMS is durable in the
M4 NVS journal. It does not wait for the cellular network.

Clients should send an `Idempotency-Key` (8..128 safe ASCII characters). The
firmware persists SHA-256(key), SHA-256(canonical SMS request) and the durable
message ID. A repeated identical request returns the existing message. Reusing
the same key for different content is `409 Conflict`. If the SMS record has
already been removed, replay also returns `409` instead of sending again.

Outbound SMS has a separate token bucket from normal API traffic. An
`uncertain` message can be retried only through `/messages/{id}/retry` with
`acknowledge_duplicate_risk: true`.

## Transport security

M5 intentionally does not ship a shared/default HTTPS private key. The embedded
HTTP server is therefore for a trusted LAN or a reverse TLS proxy. A later
milestone can provision unique per-device HTTPS credentials. Bearer tokens
must not be exposed across an untrusted network.

The OpenAPI 3.1 contract in `api/openapi.yaml` is authoritative.

## MQTT configuration

M6 adds authenticated `GET`/`PATCH /api/v1/config/mqtt`. Reads are redacted: broker passwords and custom CA PEM contents are never returned. `password_set` and `ca_configured` indicate whether those write-only values are present. See [mqtt.md](mqtt.md) for the broker/topic and Home Assistant contract.

## M6.1 API hardening

`POST /api/v1/messages` performs a codec preflight before creating any durable SMS record. Requests that cannot fit within the firmware's 16-segment GSM-7/UCS-2 limit return HTTP `422` and are never queued.

`GET /api/v1/status` exposes SMS store usage/pruning pressure, idempotency slot pressure, Huawei mode-switch counters, USB over-current/cutoff counters, and MQTT inbound replay cursor/in-flight state.

Pending two-phase idempotency reservations are intentionally not expired automatically. An authenticated operator can clear them through `POST /api/v1/system/idempotency/clear-pending` only with `{"acknowledge_duplicate_risk":true}`. This is an emergency recovery operation because a reservation may represent an SMS whose queue/finalization outcome was interrupted.

## M7 firmware endpoint

`GET /api/v1/system/firmware` reports the running/selected boot versions and
partitions, pending-verification state, confirmation scheduling and OTA counters.

`POST /api/v1/system/firmware` streams an ESP-IDF **application OTA image** using
`application/octet-stream`; it is not a multipart/form-data upload. The
`X-Firmware-SHA256` header is mandatory. Successful staging returns `202` and
the gateway reboots into the inactive slot. Reinstall/downgrade require their
explicit opt-in headers. See [ota-releases.md](ota-releases.md) for curl and
`esptool` examples and the rollback model.
