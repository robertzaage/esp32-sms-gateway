# Security policy

Please do not post real SMS content, phone numbers, SIM credentials, API tokens, Wi-Fi passwords or MQTT credentials in a public issue.

If GitHub private vulnerability reporting is enabled for this repository, use it for security reports. If it is not available, open a minimal issue asking for a private reporting channel without including vulnerability details or secrets.

## Deployment assumptions

The REST management API, including OTA, currently uses plain HTTP. It is designed for a trusted management LAN, VPN, or trusted TLS reverse proxy. Do not expose it directly to the Internet.

MQTT supports TLS and broker certificate verification. Use `mqtts://` when the broker is outside a trusted network.

The firmware protects against accidental OTA corruption with SHA-256 and uses ESP-IDF rollback, but SHA-256 over a plain-HTTP upload does not authenticate the sender or image. Deployments with a stronger threat model should add the appropriate ESP32-S3 Secure Boot, signed-image and flash-encryption controls.

More detail is in [docs/security.md](docs/security.md).
