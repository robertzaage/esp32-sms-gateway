# Project status and next steps

The software has reached the point where CI is no longer the main unknown: the repository builds successfully with the pinned ESP-IDF toolchain and the host-side parser, SMS, persistence and policy tests pass. The next useful work is hardware validation and fault testing.

## Already implemented

The current firmware includes:

- Huawei USB discovery, common storage-mode switching and AT-port probing;
- serialized AT command handling and modem registration/SIM management;
- GSM-7/UCS-2 text SMS, multipart messages and delivery reports;
- a durable SMS journal with conservative pruning and explicit uncertain-send handling;
- Wi-Fi provisioning, authenticated REST and persistent idempotency;
- MQTT with TLS support, durable incoming-event replay and Home Assistant discovery;
- USB hub support, over-current monitoring and modem recovery;
- authenticated OTA, release artifacts and ESP-IDF rollback confirmation;
- GitHub CI that performs host tests and a complete ESP32-S3 firmware build.

## Hardware validation still needed

Before calling a release stable, test the real board/modem combination for:

- cold boot with the modem attached, including its true pre-switch USB identity;
- repeated plug/unplug and USB error recovery;
- SIM PIN, registration, roaming and signal/operator changes;
- GSM-7, Unicode and multipart send/receive;
- delivery reports and power loss during an ambiguous send;
- a full/pressured SMS journal;
- Wi-Fi and MQTT outages followed by recovery/replay;
- powered-hub operation and over-current behavior;
- modem functional reset and, where available, real VBUS power cycling;
- successful OTA, bad-image rejection and deliberate crash/reset rollback;
- multi-day unattended operation.

## Likely follow-up work

These are useful, but should be driven by real hardware or deployment needs rather than added preemptively:

- raw vendor bulk AT transport if the CDC-like path proves insufficient on a supported modem;
- configurable age/count retention policy for old SMS records;
- privileged access to quarantined binary SMS if a real use case requires it;
- native HTTPS or a documented production TLS termination pattern;
- Secure Boot/signed-app/flash-encryption deployment profile;
- parser fuzzing and hardware-in-the-loop CI;
- broader modem compatibility beyond the initial Huawei target.

The project should prefer fixing observed hardware/reliability issues over adding features until the core gateway has passed a meaningful soak test.
