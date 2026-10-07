# Project status and next steps

The repository builds with the pinned ESP-IDF toolchain, the host-side parser, SMS, persistence, policy and API-contract tests pass, and the core paths work on the reference hardware. What is left before 1.0 is mostly fault testing and a long unattended run.

## Verified on hardware

With an ESP32-S3-USB-OTG powered through `USB_DEV` and a Huawei E3372 (stick firmware 21.180.01.00.00, booting as `12d1:1f01`) on a German network:

- setup portal, Wi-Fi join and the status display;
- USB mode switch to `12d1:1506`, AT port, SIM and registration;
- SMS send and receive in GSM-7 and Unicode (UCS-2, including emoji), multipart in both directions, and delivery reports for every segment;
- OTA updates over the REST API; rejection of a wrong checksum, a truncated image, a non-application image and a reinstall without the override header; automatic rollback when a new image crashes before it is confirmed (tested twice), with the crash summary recorded;
- modem restart through the API, including the VBUS power cycle, USB mode switch and SMS setup afterwards;
- MQTT over TLS (`mqtts://`, Let's Encrypt certificate) with a topic-restricted broker account;
- Home Assistant discovery with all sensors, the incoming-SMS event, the notify entity and both restart buttons (the gateway button reports `software restart: MQTT system/reboot command` afterwards).

## Before 1.0

Fault tests on the real board/modem combination:

- power-on (not only a software restart) with the modem attached;
- unplugging and replugging the modem while the gateway runs;
- Wi-Fi outage of several minutes (reconnect, setup portal fallback after 3 minutes, reconnect from the portal);
- broker restart (MQTT reconnect, replay of SMS received during the outage);
- power loss during a send (the message must end up `uncertain`, not sent twice).

Internal RAM:

- The 1.9 MB `storage` NVS partition costs an estimated ~40 KB of RAM for NVS page bookkeeping, although it holds at most 128 records. Every record is a fixed 4.6 KB blob regardless of text length. Variable-length records plus a smaller partition would free most of that RAM, at the price of a one-time factory flash. Decide before 1.0, because the partition table cannot change through OTA.

Feature checks:

- display colours, the four buttons and the display timeout.

A 7-day unattended run without unexplained restarts (`Last start` sensor) and with a stable memory low point (`Free memory low point` sensor).

## After 1.0

Useful, but driven by real needs rather than required for a release:

- Unicode/emoji-heavy and very long multipart traffic at volume;
- configurable retention for old SMS records;
- privileged access to quarantined binary SMS;
- parser fuzzing and hardware-in-the-loop CI;
- broader modem compatibility beyond the E3372;
- an optional Secure Boot / signed-image / flash-encryption profile.

## Not planned

- **HTTPS on the device.** Without PSRAM a TLS server would need another 30–40 KB of internal RAM. Use the REST API on a trusted LAN, through a VPN or behind a TLS reverse proxy.
- **A raw vendor bulk AT transport.** The CDC-style path works on the reference modem.
- **Powered-hub and roaming validation as release requirements.** The E3372 runs directly from the board's host port, and a fixed installation does not roam. Hub support stays in the firmware and the docs.
- **SIM PIN validation as a release requirement.** PIN handling is implemented; it is listed as supported but not hardware-tested until someone runs it with a PIN-protected SIM.
