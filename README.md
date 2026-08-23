# ESP32 SMS Gateway

A robust, appliance-style SMS gateway for the **Espressif ESP32-S3-USB-OTG** board and USB cellular modems. The first target modem is the Huawei `12d1:1506` composite modem/network-card device.

> **Project status:** pre-alpha foundation. The repository boots and configures the ESP32-S3-USB-OTG board for host operation. Huawei USB enumeration and `AT -> OK` are the next implementation milestone.

## Goals

- Reliable unattended SMS send/receive with persistent queues and recovery.
- USB modem state machine rather than ad-hoc AT command access.
- Versioned, authenticated REST API with an OpenAPI contract.
- MQTT with Home Assistant MQTT Discovery; no HACS dependency for core use.
- OTA with rollback and CI-produced single-file factory images.
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

The Micro-USB debug/power connector **does not power the Type-A HOST VBUS**. For the default configuration, provide 5 V to `USB_DEV` as documented by Espressif, or use a powered downstream hub and select the VBUS-off board option.

A cellular modem may exceed the board's 500 mA host limit during transmit bursts. A powered USB hub remains the recommended stability fallback.

## Toolchain

- ESP-IDF: **v6.0.2** (pinned)
- `espressif/usb_host_cdc_acm`: **2.4.0** (pinned)
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

The normal application image (`build/esp32_sms_gateway.bin`) is the OTA payload.

## Repository layout

```text
api/                    OpenAPI contract
components/board/       ESP32-S3-USB-OTG hardware control
components/modem_core/  modem lifecycle/state machine
homeassistant/          native MQTT examples
main/                   application composition root
docs/                   architecture/protocol documentation
scripts/                CI/release helpers
tests/                  host-side tests
.github/workflows/       reproducible build and release CI
```

## Development milestones

1. Foundation, CI, OTA layout, board control (**started**)
2. Huawei `12d1:1506` USB enumeration and dynamic AT-interface probing
3. Serialized AT engine + URC dispatcher
4. SIM/network modem manager and progressive recovery
5. PDU SMS receive/send, multipart and delivery reports
6. Durable inbox/outbox and deduplication
7. REST API v1 + authentication
8. MQTT contract + Home Assistant Discovery
9. Provisioning, OTA delivery and browser installer
10. Soak/fault-injection and hardware-in-the-loop testing

See [docs/architecture.md](docs/architecture.md) and [docs/roadmap.md](docs/roadmap.md).

## Security / privacy

SMS bodies, phone numbers, API tokens, MQTT credentials and SIM PINs must not be emitted at normal log levels. Raw AT access will be diagnostic-only, privileged and disabled by default.

See [SECURITY.md](SECURITY.md).

## License

No project license has been selected yet. Choose one before public distribution/contributions are accepted.
