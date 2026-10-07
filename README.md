# ESP32 SMS Gateway

[![CI](https://github.com/robertzaage/esp32-sms-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/robertzaage/esp32-sms-gateway/actions/workflows/ci.yml)

ESP32 SMS Gateway turns an **Espressif ESP32-S3-USB-OTG** board and a USB cellular modem into a small, local SMS appliance. It can send and receive text messages through REST or MQTT, integrates with Home Assistant, keeps important message state in flash, and supports rollback-safe OTA updates.

The first supported modem family is Huawei. The reference modem is a Huawei E3372 with "stick" (serial modem) firmware: it boots as USB storage `12d1:1f01` and is switched to its AT-capable modem mode `12d1:1506`. HiLink firmware (`12d1:14dc`) has no AT port and is not supported.

The firmware builds successfully in GitHub Actions with ESP-IDF 6.0.2. Physical modem testing is still important before relying on it for unattended or safety-critical use.

## What it does

- Sends and receives GSM-7 and Unicode text SMS, including multipart messages.
- Stores message state in flash so reboots do not silently lose the queue.
- Avoids automatic resend when the modem may already have accepted a message.
- Provides an authenticated REST API with an OpenAPI 3.1 contract.
- Publishes and accepts MQTT messages with persistent request idempotency.
- Uses native Home Assistant MQTT discovery; no HACS component is required.
- Handles Wi-Fi reconnects, modem recovery, USB reconnects, powered hubs and common Huawei storage-mode personalities.
- Accepts authenticated OTA application images and uses ESP-IDF rollback if a new image does not stay healthy.
- Produces a merged factory image that can be flashed with `esptool`.

## Hardware

The reference board is the **ESP32-S3-USB-OTG** with 8 MB flash.

Connect the board's debug/programming USB port to your computer. The Type-A host socket also needs a usable 5 V host supply; with the default board configuration this comes from the `USB_DEV` input. See [hardware bring-up](docs/hardware-bringup.md) before attaching the modem.

Cellular modems can draw short current bursts above the board's 500 mA host limit. If the modem resets or disappears during registration or transmission, use a good powered USB 2.0 hub. Keep upstream VBUS enabled unless you have tested your hub with a different topology. When a hub supplies downstream power, the ESP32 usually cannot perform a true modem power cycle.

## Flash a release

Install `esptool`:

```sh
python -m pip install esptool
```

Download the `*-factory.bin` file from a GitHub release and flash it at address `0x0`:

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x0 esp32-sms-gateway-vX.Y.Z-factory.bin
```

Use the `*-ota.bin` file only for OTA updates. Details are in [OTA and releases](docs/ota-releases.md).

## First boot

On a fresh device the display shows the setup Wi-Fi name (`SMS-Gateway-XXXXXX`) and its password. Join that network; most phones then open the setup page automatically, otherwise browse to `http://192.168.4.1`. The page lets you:

- pick your Wi-Fi network from a scan and enter its password;
- optionally enter your MQTT broker (host, port, TLS, username, password), enable Home Assistant discovery and set a default SMS recipient for the Home Assistant notify entity;
- optionally set a REST API token. Leave it empty and the gateway generates one and shows it once on the page.

When you save, the gateway tests the Wi-Fi credentials and shows the result on the page and the display. If they work, it restarts and joins your network. If they don't, the portal stays open so you can correct them.

The setup password is generated once per device and stays the same across reboots. The portal also reopens on its own when the saved Wi-Fi has been unreachable for three minutes, while the gateway keeps retrying that network. To change settings later, hold the **MENU** button for five seconds.

### Display

The display shows Wi-Fi, MQTT and modem state, signal strength and the last received SMS. It switches off after 60 seconds without new content and lights up again when something changes, an SMS arrives, or any button is pressed. The image moves by a few pixels on every wake to spread pixel wear. Timeouts and the SMS preview are configurable under `Gateway display` in `idf.py menuconfig`.

Check the gateway:

```sh
curl http://sms-gateway.local/api/v1/health

curl \
  -H "Authorization: Bearer $TOKEN" \
  http://sms-gateway.local/api/v1/status
```

Send a message:

```sh
curl --fail-with-body \
  -X POST http://sms-gateway.local/api/v1/messages \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: example-send-0001' \
  -d '{"to":"+491701234567","text":"Hello from ESP32","delivery_report":true}'
```

The API returns after the message is durably queued; cellular delivery continues asynchronously.

## MQTT and Home Assistant

MQTT is optional and disabled until configured. It supports `mqtt://` and CA-verified `mqtts://`, retained availability/status, structured SMS commands, incoming SMS events and Home Assistant device discovery.

Start with [MQTT](docs/mqtt.md) and [Home Assistant](homeassistant/README.md).

## Build from source

The project pins ESP-IDF **v6.0.2** and its managed component versions. With ESP-IDF active in your shell:

```sh
idf.py set-target esp32s3
idf.py build
```

For development flashing you can use the normal ESP-IDF commands. Release and recovery users do not need the full toolchain; the merged factory image is intended for `esptool`.

Run the host-side checks with:

```sh
python scripts/validate_contracts.py
python -m unittest discover -s tests -v
```

GitHub Actions also performs a full ESP-IDF target build and assembles flashable artifacts.

## Documentation

- [Hardware bring-up](docs/hardware-bringup.md) — wiring, power, first modem test and troubleshooting.
- [Networking and REST API](docs/networking-api.md) — provisioning, authentication and API examples.
- [MQTT](docs/mqtt.md) — topics, commands, delivery behavior and Home Assistant discovery.
- [OTA and releases](docs/ota-releases.md) — factory flashing, OTA updates and rollback.
- [SMS behavior](docs/sms-subsystem.md) — encoding, persistence, multipart messages and retry semantics.
- [Security and privacy](docs/security.md) — deployment assumptions and secret handling.
- [Architecture](docs/architecture.md) — a concise developer overview.
- [Roadmap](docs/roadmap.md) — current validation and hardening work.

## Security note

The REST management listener is plain HTTP. Treat it as a trusted-LAN interface or put it behind a trusted VPN/TLS reverse proxy. Do not expose the bearer token or OTA endpoint directly to the Internet. MQTT can use TLS and should use `mqtts://` when the broker is outside a trusted network.

## License

Licensed under the [MIT License](LICENSE).
