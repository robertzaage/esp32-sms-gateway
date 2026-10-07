# Changelog

This project uses Semantic Versioning. The 0.7.0 alpha releases are hardware bring-up builds for the ESP32-S3-USB-OTG with a Huawei E3372.

## Unreleased

## 0.7.0-alpha.16 - 2026-10-07

### Fixed

- Internal RAM briefly dropped to 1–5 KB, even when idle. Measured with a windowed low-point probe:
  - LAN traffic bursts filled up to 32 dynamic Wi-Fi RX buffers (~50 KB). The limit is now 12.
  - Every MQTT work item reserved a fixed 4.6 KB payload buffer, so a burst of SMS status events could pin ~40 KB. Items are now sized to their content.
  - Sending reserved segment buffers for 16 segments (6 KB) instead of the message's actual segment count.

  Idle windows now stay above ~40 KB; a 3-part SMS round trip to the gateway's own number bottoms out at ~13 KB instead of ~2 KB.

### Added

- Home Assistant examples: forward incoming SMS as a phone notification, send through the notify entity, and a dashboard card.
- Release notes are taken from this changelog, and prerelease tags are published as GitHub prereleases.
- README quick start.

### Changed

- Roadmap split into what is left before 1.0, what can wait and what is not planned.

## 0.7.0-alpha.14 - 2026-10-07

### Added

- MQTT status and Home Assistant diagnostics: uptime, free memory, its low point, the largest free block and the last start reason (for example `software restart: firmware update installed`).
- Tests that every REST route and every `/status` and `/config/mqtt` field appears in `api/openapi.yaml`, and host tests for the AT command checks.

### Changed

- SMS setup tries `AT+CNMI=2,1,0,2,0` first (the E3372 rejects the previous first choice). Errors from rejected fallback variants are cleared once setup succeeds, so `sms.last_error` no longer shows `ESP_FAIL` on a healthy gateway.
- The AT diagnostics endpoint matches lower-case commands to the modem's upper-case response prefix.
- Documentation brought in line with the hardware-tested firmware. Among other things, there is no `sms-gateway.local` (no mDNS): use the IP shown on the display.

## 0.7.0-alpha.13 - 2026-10-07

### Fixed

- `mqtts://` failed with `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED`. With Wi-Fi, the USB host and the modem stack running, only ~29 KB of internal RAM was free (largest block 6 KB, low point 1.3 KB). TLS record buffers are now allocated dynamically, Wi-Fi code runs from flash instead of IRAM, the static Wi-Fi RX buffers went from 10 to 6 and the SMS URC queue from 32 to 8 entries. With an open TLS session ~66 KB is free (largest block 31 KB).

### Added

- `/api/v1/status` reports internal RAM as `gateway.heap` (`free`, `largest_block`, `minimum_free`).

0.7.0-alpha.12 was an untagged test build of the same changes.

## 0.7.0-alpha.11 - 2026-10-07

### Added

- Every deliberate restart stores its source; `/api/v1/status` reports it as `gateway.restart_cause`.
- The OpenAPI contract covers the AT diagnostics endpoint, the gateway diagnostics and the SMS setup fields.

## 0.7.0-alpha.10 - 2026-10-07

### Fixed

- Incoming SMS and delivery reports were only picked up at startup: Huawei sends `+CMTI`/`+CDS` to the PC UI port by default, and the gateway listens on the modem port. The gateway now sets `AT^PORTSEL=1` and also polls the modem inbox every 60 s.

### Added

- `POST /api/v1/modem/at` diagnostic endpoint (token required). `/api/v1/status` shows `sms.inbox_scans` and `sms.inbox_scan_failures`.

## 0.7.0-alpha.9 - 2026-10-07

### Fixed

- Outgoing SMS could stay `queued` forever: if the SMS setup (`AT+CMGF=0`, `AT+CNMI`) failed once after the modem became ready, it was never retried. It is now retried every 30 s, with fallback `AT+CNMI` modes. `/api/v1/status` shows `sms.pdu_mode_configured` and the failing setup command.
- `POST /api/v1/messages` rejected the documented `delivery_report` field (it only accepted `request_delivery_report`). Both are accepted now, matching the MQTT command.

## 0.7.0-alpha.8 - 2026-10-07

### Fixed

- The modem was never powered: the over-current input (GPIO21, MIC2005A `FAULT/`) is active low, but was read as active high, so the gateway cut the `USB_HOST` power at boot and kept it off. Short faults (plug-in inrush) are now ignored.
- `USB_SEL` (GPIO18) must be high to route the ESP32-S3 USB pins to the `USB_HOST` socket (user guide, esp-bsp). 0.7.0-alpha.6 had wrongly changed it to low.

## 0.7.0-alpha.7 - 2026-10-07

### Fixed

- Wi-Fi start failed with out-of-memory, aborting boot (`app_main` → `network_service_init`). The display's 115 KB full-frame buffer left only ~55–78 KB of internal RAM. The display now draws in 24-line strips (11.5 KB), and about 180 KB is free before Wi-Fi starts.

### Added

- A failing boot step is recorded with its name, error code and free heap, and shown as the crash summary on the next boot.

## 0.7.0-alpha.6 - 2026-10-07

### Added

- Crash diagnostics: a core dump goes to flash, and a crash summary appears on the display, in `/api/v1/status` and in the portal. Safe mode starts after repeated crash resets. The firmware ELF is published with CI and release builds.

### Changed

- Display or modem initialization errors no longer reboot the gateway.
- The console is UART0 (Micro-USB) only.
- New 56 KB `coredump` partition; flash once with the factory image when coming from an older release.

## 0.7.0-alpha.5 - 2026-10-07

### Fixed

- Boot loop: startup overflowed the 3.5 KB main task stack (MQTT settings copies of ~3.8 KB each), so the gateway crashed and restarted before Wi-Fi could connect. The display flashed on every restart.
- Wi-Fi never retried after a failed first connection from the setup portal, and three disconnects at any point switched the gateway into setup mode for good.
- The setup Wi-Fi password was shown in uppercase although it contained lowercase letters.
- Huawei E3372 sticks booting as `12d1:1f01` were not switched to modem mode.
- Display colors were byte-swapped.

### Changed

- Setup portal: Wi-Fi scan, MQTT/Home Assistant settings, optional generated API token, a live connection test, a stable setup password, automatic fallback after 3 minutes offline, and MENU long-press to reopen.
- Display: status page with Wi-Fi, MQTT, modem, signal and last SMS. It switches off after a timeout and wakes on new content or a button press, with a pixel shift on each wake.
- Huawei periodic status reports (`^RSSI` and similar) are disabled with `AT^CURC=0`.
- Removed the unused `espressif/network_provisioning` dependency.

## 0.7.0-alpha.2 to 0.7.0-alpha.4 - 2026-09-15

- Web setup portal and status display introduced; documentation rewritten; MIT license added.
- The setup portal reopens after Wi-Fi failures.

## 0.7.0-alpha.1 - 2026-08-25

First tagged prerelease. The development history before it is available in Git and is not repeated here.
