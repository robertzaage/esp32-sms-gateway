# Security and privacy

- Never log SMS bodies, recipients, senders, SIM PINs, API bearer tokens, MQTT
  passwords, broker private CAs, or IMEI at normal log levels.
- The generated REST bearer token is shown once on the serial console at first
  creation; only its SHA-256 digest is persisted.
- Wi-Fi onboarding uses `network_provisioning` Security 1 with a random,
  transient proof-of-possession value.
- The REST listener is plain HTTP by design. Keep it on a trusted LAN or put
  it behind a TLS reverse proxy. Do not expose it directly to the Internet.
- MQTT permits only `mqtt://` and `mqtts://`; TLS mode verifies the broker with a configured private CA or the Espressif certificate bundle. Credentials are separate from the URI and are not logged.
- MQTT configuration reads are redacted: password and CA content are write-only, with only presence flags returned.
- Raw arbitrary AT execution is not exposed through REST or MQTT.
- SIM PIN values remain volatile and their command buffers are explicitly
  wiped after submission.
- SMS storage is intentionally durable; physical flash confidentiality requires
  the ESP32-S3 secure-boot/flash-encryption deployment options appropriate to
  the threat model.
- Incoming SMS text published to Home Assistant in M6 can be retained by Home
  Assistant Recorder. Exclude the SMS event/entity if message-content history
  is not desired.

## MQTT TLS credential lifetime

ESP-MQTT retains a pointer to a configured broker CA certificate instead of copying it. The MQTT service therefore owns a dedicated CA PEM allocation for the entire client lifetime and wipes/frees it only after `esp_mqtt_client_stop()` / `esp_mqtt_client_destroy()` complete. Runtime reconfiguration must preserve this ownership rule.

## M6.1 command and recovery safeguards

- Retained MQTT messages are never accepted as SMS/restart commands. Restart actions require the exact non-retained payload `PRESS`.
- Pending idempotency reservations are not expired automatically because their send outcome may be unknown. `/api/v1/system/idempotency/clear-pending` requires explicit duplicate-risk acknowledgement and should be used only by an operator who understands that clearing a reservation can permit a duplicate SMS.
- Incoming MQTT SMS events are at-least-once. Applications must tolerate duplicate event notifications after acknowledgement loss; the durable SMS record ID is the deduplication key.
- REST remains HTTP on the trusted LAN in this milestone. Do not expose it directly to an untrusted network; prefer `mqtts://` or terminate HTTPS at a trusted local reverse proxy until native HTTPS provisioning is added.

## M7 OTA security boundary

The firmware update endpoint is bearer-authenticated and requires a SHA-256 of
the exact OTA application binary. ESP-IDF then validates the staged application
image before `esp_ota_set_boot_partition()` is allowed to select it. The gateway
also rejects wrong-project images, accidental reinstall/downgrade, malformed
SemVer and a lower application `secure_version`.

The SHA-256 header is an integrity/correct-file safeguard, **not cryptographic
authentication when REST is plain HTTP**. An active attacker on that network
could replace both the upload and its digest. Perform OTA only on a trusted
management LAN or through trusted TLS/VPN termination until native HTTPS or
signed-app enforcement is deployed. Never expose the OTA route directly to the
Internet.

Rollback confirmation is deliberately delayed: a new OTA image must initialize
all critical services and remain alive through the configured stability window
before it is marked valid. A reset before confirmation causes ESP-IDF rollback.
