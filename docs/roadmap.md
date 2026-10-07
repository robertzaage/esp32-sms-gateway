# Project status and next steps

The repository builds with the pinned ESP-IDF toolchain, the host-side parser, SMS, persistence and policy tests pass, and the core paths work on the reference hardware. The next useful work is fault and long-running testing.

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

## Verified on hardware

With an ESP32-S3-USB-OTG powered through `USB_DEV` and a Huawei E3372 (stick firmware 21.180.01.00.00, booting as `12d1:1f01`) on a German network, the following work:

- setup portal, Wi-Fi join and the status display;
- USB mode switch to `12d1:1506`, AT port, SIM and registration;
- GSM-7 SMS send, receive and delivery reports;
- OTA updates over the REST API;
- MQTT over TLS (`mqtts://`, Let's Encrypt certificate) with a topic-restricted broker account.

## Hardware validation still needed

Before calling a release stable, test the real board/modem combination for:

- cold boot with the modem attached;
- repeated plug/unplug and USB error recovery;
- SIM PIN, registration, roaming and signal/operator changes;
- Unicode and multipart send/receive;
- power loss during an ambiguous send;
- a full/pressured SMS journal;
- Wi-Fi and MQTT outages followed by recovery/replay;
- Home Assistant discovery, notify entity and restart buttons;
- powered-hub operation and over-current behavior;
- modem functional reset and, where available, real VBUS power cycling;
- bad-image rejection and deliberate crash/reset rollback;
- multi-day unattended operation, including the internal RAM low point (`gateway.heap.minimum_free`).

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
