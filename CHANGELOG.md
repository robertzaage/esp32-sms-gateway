# Changelog

This project uses Semantic Versioning for tagged releases.

## Unreleased

The current development line is preparing the first public hardware-tested release. Notable user-facing changes made after a release will be collected here until the next tag.

### Fixed

- Wi-Fi start failed with out-of-memory, aborting boot (`app_main` → `network_service_init`). The display's 115 KB full-frame buffer left only ~55–78 KB of internal RAM. The display now draws in 24-line strips (11.5 KB), and about 180 KB is free before Wi-Fi starts.
- A failing boot step is recorded with its name, error code and free heap, and shown as the crash summary on the next boot.
- The modem was never powered: the over-current input (GPIO21, MIC2005A `FAULT/`) is active low, but was read as active high, so the gateway cut the `USB_HOST` power at boot and kept it off. Short faults (plug-in inrush) are now ignored.
- `USB_SEL` (GPIO18) must be high to route the ESP32-S3 USB pins to the `USB_HOST` socket (user guide, esp-bsp). 0.7.0-alpha.6 had wrongly changed it to low.
- Display or modem initialization errors no longer reboot the gateway.
- Boot loop: startup overflowed the 3.5 KB main task stack (MQTT settings copies of ~3.8 KB each), so the gateway crashed and restarted before Wi-Fi could connect. The display flashed on every restart.
- Wi-Fi never retried after a failed first connection from the setup portal, and three disconnects at any point switched the gateway into setup mode for good.
- The setup Wi-Fi password was shown in uppercase although it contained lowercase letters.
- Huawei E3372 sticks booting as `12d1:1f01` were not switched to modem mode.
- Display colors were byte-swapped.

### Changed

- Crash diagnostics: a core dump goes to flash, and a crash summary appears on the display, in `/api/v1/status` and in the portal. Safe mode starts after repeated crash resets. The firmware ELF is now published with CI and release builds.
- The console is UART0 (Micro-USB) only.

- Setup portal: Wi-Fi scan, MQTT/Home Assistant settings, optional generated API token, a live connection test, a stable setup password, automatic fallback after 3 minutes offline, and MENU long-press to reopen.
- Display: status page with Wi-Fi, MQTT, modem, signal and last SMS. It switches off after a timeout and wakes on new content or a button press, with a pixel shift on each wake.
- Huawei periodic status reports (`^RSSI` and similar) are disabled with `AT^CURC=0`.
- Removed the unused `espressif/network_provisioning` dependency.

## Before the first release

The gateway was developed iteratively while the USB, modem, SMS, networking, MQTT and OTA pieces were brought together. That development history remains available in Git and is intentionally not repeated in this file.

For the current feature set and known validation gaps, see [README.md](README.md) and [docs/roadmap.md](docs/roadmap.md).
