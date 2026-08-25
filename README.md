# ESP32 SMS Gateway

A robust, appliance-style SMS gateway for the **Espressif ESP32-S3-USB-OTG** board and USB cellular modems. The first target modem is the Huawei `12d1:1506` composite modem/network-card device.

> **Project status:** pre-alpha / M7 OTA and releases. USB/AT/modem/SMS are joined by secure SoftAP Wi-Fi provisioning, authenticated REST, TLS-capable MQTT, native Home Assistant MQTT Device Discovery, durable inbound-event replay, Huawei cold-boot mode switching, and pre-hardware reliability guards. M7 authenticated OTA/release packaging is implemented. A hosted ESP-IDF CI run and physical-board verification are still required.

## Goals

- Reliable unattended SMS send/receive with persistent queues and recovery.
- USB modem state machine rather than ad-hoc AT command access.
- Versioned, authenticated REST API with an OpenAPI contract.
- MQTT with Home Assistant MQTT Discovery; no HACS dependency for core use.
- Authenticated push OTA with delayed rollback confirmation, plus CI-produced OTA/factory images.
- Privacy-conscious logging and bounded persistent storage.
- Host-side parser/PDU tests plus later hardware-in-the-loop tests.

## Hardware

Target board: **ESP32-S3-USB-OTG**, ESP32-S3-MINI-1-N8 (8 MB flash).

The board's USB Type-A female port requires explicit firmware control:

- GPIO18: route ESP32-S3 USB D+/D- to the Type-A HOST connector.
- GPIO17: enable the 500 mA current limiter.
- GPIO12: enable host VBUS from the USB_DEV 5 V input.
- GPIO13: alternatively enable the battery boost VBUS source.
- GPIO21: over-current input.

With a board-owned VBUS source, firmware can genuinely power-cycle the Type-A port by disabling both source enables and later re-enabling the configured path. With `GATEWAY_USB_HOST_POWER_OFF`, a powered hub owns downstream VBUS and firmware correctly reports hard power reset as unavailable.

The Micro-USB debug/power connector **does not power the Type-A HOST VBUS**. For the default configuration, provide 5 V to `USB_DEV` as documented by Espressif.

A cellular modem may exceed the board's 500 mA host limit during transmit bursts. A powered USB 2.0 hub remains the recommended stability fallback and USB hub support is enabled in the firmware. Do not assume `GATEWAY_USB_HOST_POWER_OFF` is appropriate for every hub: some self-powered hubs still expect upstream VBUS for attach/session detection. Keep the board's USB_DEV host VBUS enabled unless the chosen hub has been verified with the VBUS-off topology. A hub-powered modem generally cannot be hard power-cycled by the board GPIOs.

## Toolchain

- ESP-IDF: **v6.0.2** (pinned)
- `espressif/usb_host_cdc_acm`: **2.4.0** (pinned)
- `espressif/network_provisioning`: **1.2.4** (pinned)
- `espressif/mqtt`: **1.1.0** (pinned)
- `espressif/cjson`: **1.7.19~2** (pinned)
- Target: `esp32s3`

## Build

```sh
idf.py set-target esp32s3
idf.py build
```

Create one binary for initial/recovery flashing:

```sh
idf.py merge-bin -o esp32-sms-gateway-factory.bin -f raw
```

The normal application image (`build/esp32_sms_gateway.bin`) is the OTA payload. For a release factory image, flash with `python -m esptool --chip esp32s3 --port PORT write-flash 0x0 <factory.bin>`. See [OTA and releases](docs/ota-releases.md).

## Repository layout

```text
api/                    OpenAPI contract
components/board/       ESP32-S3-USB-OTG hardware control
components/modem_core/  composition and externally visible modem lifecycle
components/modem_manager/ SIM/network/status policy and recovery
components/at_engine/   serialized AT transactions, tokenizer and URC routing
components/sms_codec/   host-testable GSM-7/UCS-2/PDU codec
components/sms_store/   versioned durable NVS SMS journal
components/sms_service/ incoming/outgoing SMS state machine
components/usb_modem_transport/ USB Host, Huawei discovery and AT-port probe
components/network_service/ Wi-Fi provisioning/reconnect/SNTP
components/gateway_security/ bearer-token generation/hash validation
components/api_common/     host-testable API validation/rate limits
components/api_idempotency/ persistent REST/MQTT retry safety
components/api_server/     authenticated REST v1 handlers
components/gateway_settings/ persistent MQTT configuration
components/mqtt_service/    TLS MQTT, commands/events and HA discovery
components/ota_service/     streamed OTA validation, version policy and rollback confirmation
homeassistant/          native MQTT examples
main/                   application composition root
docs/                   architecture/protocol documentation
scripts/                CI/release helpers
tests/                  host-side tests
.github/workflows/       pinned build and release CI
```

## Development milestones

1. Foundation, CI, OTA layout, board control (**implemented; hosted CI pending**)
2. Huawei `12d1:1506` USB enumeration and dynamic AT-interface probing (**implemented; hardware verification pending**)
3. Serialized AT engine + URC dispatcher (**implemented; target compile/hardware integration pending**)
4. SIM/network modem manager and progressive recovery (**implemented; target/hardware verification pending**)
5. PDU text-SMS receive/send, multipart, delivery reports and durable NVS journal (**implemented; target/hardware verification pending**)
6. Wi-Fi provisioning + REST API v1 + authentication (**implemented; target/hardware verification pending**)
7. MQTT contract + Home Assistant Discovery (**implemented; target/hardware verification pending**)
8. M6.1 pre-hardware hardening: retained-command rejection, durable event replay, storage pressure, hub/overcurrent handling, CDC reprobe and Huawei mode switching (**implemented; target/hardware verification pending**)
9. Authenticated OTA + rollback + esptool/release artifacts (**implemented; hosted CI/hardware rollback tests pending**)
10. Soak/fault-injection and hardware-in-the-loop testing

See [docs/architecture.md](docs/architecture.md), [docs/roadmap.md](docs/roadmap.md), [docs/at-engine.md](docs/at-engine.md), [docs/modem-manager.md](docs/modem-manager.md), [docs/sms-subsystem.md](docs/sms-subsystem.md), [docs/networking-api.md](docs/networking-api.md), [docs/ota-releases.md](docs/ota-releases.md), and the [M1 hardware bring-up guide](docs/hardware-bringup.md).

## Security / privacy

SMS bodies, phone numbers, API tokens, MQTT credentials and SIM PINs must not be emitted at normal log levels. Raw AT access will be diagnostic-only, privileged and disabled by default.

See [SECURITY.md](SECURITY.md).

## License

No project license has been selected yet. Choose one before public distribution/contributions are accepted.
