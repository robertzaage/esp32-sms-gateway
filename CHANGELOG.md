# Changelog

This project uses Semantic Versioning for tagged releases.

## Unreleased

The current development line is preparing the first public hardware-tested release. Notable user-facing changes made after a release will be collected here until the next tag.

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

## Before the first release

The gateway was developed iteratively while the USB, modem, SMS, networking, MQTT and OTA pieces were brought together. That development history remains available in Git and is intentionally not repeated in this file.

For the current feature set and known validation gaps, see [README.md](README.md) and [docs/roadmap.md](docs/roadmap.md).
