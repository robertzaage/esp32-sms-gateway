# Changelog

All notable changes will be documented here. The project intends to follow Semantic Versioning once public releases begin.

## Unreleased

### M7 - OTA and release hardening

- Added authenticated streamed OTA upload with required SHA-256 validation.
- Added project/SemVer/reinstall/downgrade/secure-version image policy.
- Replaced immediate OTA confirmation with a delayed post-service stability window.
- Added OTA diagnostics and OpenAPI 0.7.0 firmware endpoints.
- Added release image metadata inspection and deterministic release manifest generation.
- Updated GitHub Actions to current checkout/setup-python/upload-artifact major versions while keeping ESP-IDF 6.0.2 pinned.
- Tagged releases now verify the embedded app version, publish OTA + merged factory images, checksums and manifest.
- Added esptool-only initial/recovery flashing and OTA operator documentation; no browser installer is used.

### Added

- Initial ESP-IDF project foundation for ESP32-S3-USB-OTG.
- Board USB host routing, VBUS-source selection and over-current input.
- Dual-slot OTA partition layout with rollback confirmation hook.
- Initial modem lifecycle state vocabulary.
- OpenAPI v1, MQTT and Home Assistant integration drafts.
- CI/release workflows that produce OTA and merged factory images.
- USB Host + CDC-like Huawei transport for `12d1:1506`.
- Live descriptor scanning and ranked serial-interface discovery with NCM alt-setting exclusion.
- Dynamic `AT\r` / `OK` probing, disconnect handling, diagnostics counters, and reconnect-ready transport lifecycle.
- Host-compiled regression test reproducing the observed Huawei composite USB layout.
- M1 physical-board bring-up and troubleshooting guide.
- Transport-independent serialized AT transaction engine with bounded request/RX/URC queues.
- Incremental host-tested tokenizer, CME/CMS/final-result parsing and response capture.
- Context-sensitive atomic `>` prompt transactions suitable for later `AT+CMGS` SMS submission.
- URC routing with expected-prefix precedence and multi-line `+CMT`/`+CDS`/`+CBM` continuation handling.
- Per-command deadlines, cancellation, opt-in retry policy, transport-loss aborts and diagnostics.
- Deterministic random-fragmentation and interleaved-URC regression tests.
- M2 AT-engine architecture and safety documentation.
- M3 single-writer modem manager with SIM, registration, signal, operator and identity snapshots.
- `ATE0`/`CMEE`, registration-URC setup and compatibility fallbacks for older modem firmware.
- Volatile SIM PIN submission with validation and command-buffer zeroization.
- Host-tested `+CPIN`, `+CSQ`, `+CREG`/`+CGREG`/`+CEREG` and `+COPS` parsers.
- Deterministic recovery policy: AT-profile reinit, `CFUN` reset, then board VBUS cycle.
- Stable-poll recovery-ladder reset and explicit degraded behavior when VBUS control is unavailable.
- M4 GSM-7/UCS-2 PDU codec with Unicode surrogate-pair handling, alphanumeric originators and multipart UDH.
- Durable NVS-backed SMS journal with versioned fixed-width records and host fake-NVS regression tests.
- PDU-mode receive service for `+CMTI`/`+CDSI`, direct `+CMT`/`+CDS`, restart inbox draining and deduplication.
- Durable outbound `AT+CMGS` segmentation, per-segment TP-MR tracking and delivery-report correlation.
- Conservative `uncertain` outbound state after ambiguous interruption; explicit retry only, never automatic duplicate-prone resend.
- Board power-control capability/source API distinguishing board-owned Type-A VBUS from externally powered-hub operation.
- M5 secure SoftAP Wi-Fi onboarding using the ESP-IDF 6 `network_provisioning` managed component and Security 1 proof-of-possession.
- Wi-Fi station reconnect with capped exponential backoff, hostname/device identity and SNTP synchronization.
- 256-bit REST bearer-token generation with SHA-256-only persistence and constant-time validation.
- Authenticated REST v1 status, SMS list/send/get/delete/retry, SIM PIN, modem restart and system reboot handlers.
- Bounded persistent HTTP `Idempotency-Key` cache with request fingerprint conflict detection and conservative replay behavior.
- Host-tested E.164 validation and token-bucket rate limiting, including a stricter outbound-SMS bucket.
- Networking/API security and provisioning documentation.
- M6 persistent MQTT broker/TLS/base-topic/Home Assistant configuration with redacted REST management.
- ESP-MQTT QoS/LWT/reconnect service with bounded fragmented-payload reconstruction and worker-task command isolation.
- Shared persistent REST/MQTT request idempotency for structured SMS sends.
- Retained non-sensitive MQTT status plus non-retained SMS receive/status event topics.
- Home Assistant MQTT Device Discovery with required origin/device metadata, diagnostic entities, incoming-SMS event and restart buttons.
- Optional native MQTT notify entity for one configured default recipient plus an idempotent arbitrary-recipient Home Assistant script.
- Broker CA verification using a configured private CA or Espressif's certificate bundle; broker credentials are kept out of URI/log output.
- MQTT custom-CA lifetime ownership: private CA PEM is held in a dedicated client-lifetime allocation and wiped only after the MQTT client is destroyed.
- Home Assistant Recorder/SMS-content privacy guidance.

### M6.1 hardening

- Reject retained MQTT application commands and require exact `PRESS` payloads for restart actions.
- Replay durable inbound SMS events over MQTT QoS 1 after reconnect; retain a persisted acknowledgement cursor and protect unpublished inbox records from pruning.
- Add conservative SMS-store pressure pruning, capacity/prune diagnostics, and preserve all active/ambiguous outbound records.
- Add explicit, risk-acknowledged recovery for stranded idempotency reservations rather than silently expiring them.
- Preflight outbound GSM-7/UCS-2 segmentation at REST/MQTT boundaries and reject messages above the 16-segment limit before queueing.
- Enable ESP-IDF USB hub support for powered-hub deployments.
- Continuously monitor USB host over-current, cut board-owned VBUS on fault, and restore only after a cooldown.
- Reprobe after CDC transport errors and ignore stale callbacks using per-open generation tokens.
- Add Huawei storage-personality mode switching for `12d1:1446`, `12d1:14fe`, and one configurable additional PID before normal `12d1:1506` AT probing.
- Quarantine unsupported compressed and GSM national-language-shift SMS instead of decoding them incorrectly.
- Update the OpenAPI contract to v0.6.1 with hardening diagnostics and recovery semantics.
