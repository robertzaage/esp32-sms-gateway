# Security policy

This project is pre-alpha and is not yet suitable for exposure to an untrusted network.

When reporting a vulnerability, do not include real SMS bodies, phone numbers, SIM credentials, API tokens, Wi-Fi passwords or MQTT credentials in a public issue. Use a private security-reporting channel once the GitHub repository enables one.

Security-sensitive design constraints are documented in `docs/security.md`.

M7 adds a bearer-authenticated OTA upload route. OTA is still served over the
same plain-HTTP trusted-LAN management listener, so SHA-256 verifies integrity
but not network authenticity. Do not expose OTA to an untrusted network; use a
trusted TLS reverse proxy/VPN or deploy ESP32 signed-app/Secure Boot controls
for a stronger threat model. See `docs/ota-releases.md` and `docs/security.md`.
