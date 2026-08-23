# Security design notes

- API management endpoints require authentication once networking is implemented.
- API tokens are stored as verifiers/hashes where feasible; plaintext values are only shown at creation time.
- MQTT supports authenticated TLS and CA validation.
- Raw AT execution is disabled by default and never part of the ordinary public API.
- Phone numbers and SMS bodies are redacted from INFO/WARN/ERROR logs.
- SIM PINs, Wi-Fi credentials, MQTT credentials and API tokens are never logged.
- Request bodies and MQTT payloads have explicit maximum sizes.
- SMS sending has queue/rate limits and idempotency controls.
- OTA accepts only compatible images and uses rollback; signed firmware is a planned hardening option.
- Persistent SMS retention is bounded and configurable.
