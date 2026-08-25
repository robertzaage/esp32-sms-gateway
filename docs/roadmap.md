# Roadmap

## M0 - Foundation

- [x] ESP-IDF v6.0.2 pin
- [x] ESP32-S3 target and 8 MB OTA partition layout
- [x] ESP32-S3-USB-OTG host routing/power abstraction
- [x] modem lifecycle state vocabulary
- [x] CI build + merged factory artifact
- [x] OpenAPI v1 draft
- [x] MQTT/Home Assistant contract draft
- [ ] first CI run in hosted GitHub repository

## M1 - USB modem proof

Implementation is present; checklist remains open until verified on the physical board.

- [x] install USB Host service/task (implementation; hardware acceptance pending)
- [x] detect connect/disconnect with stale-callback generation protection (implementation; hardware acceptance pending)
- [x] identify `12d1:1506` and common Huawei pre-switch storage personalities
- [x] enumerate candidate serial interfaces
- [x] attempt `usb_host_cdc_acm` vendor-specific open
- [ ] fallback raw bulk transport when required
- [x] dynamic `AT\r` probe and `OK` detection
- [x] expose USB descriptor/selected interface and mode-switch diagnostics
- [ ] tolerate repeated physical unplug/replug

Acceptance: 1,000 automated VBUS cycles without needing an ESP reboot, subject to modem power stability.

## M2 - AT engine

Implementation complete; target-side compile and physical integration remain gated by hosted CI/hardware.

- [x] single serialized command queue
- [x] incremental CR/LF tokenizer
- [x] response vs URC routing
- [x] `OK`, `ERROR`, `+CME ERROR`, `+CMS ERROR`, atomic prompt handling
- [x] per-command deadlines/cancellation/retry policy
- [x] random packet-fragmentation host tests
- [x] interleaved and multi-line URC tests

Acceptance: concurrent callers cannot interleave writes; arbitrary RX fragmentation produces the same token stream; transport loss/cancel/overflow cannot yield a false successful result.

## M3 - Modem manager

Implementation complete; target-side compile and physical behavior remain gated by hosted CI/hardware.

- [x] manufacturer/model/revision/IMEI identification (`CGMI`/`CGMM`/`CGMR`/`CGSN`)
- [x] SIM/PIN state with volatile PIN submission and zeroization
- [x] CSQ validation and RSSI-to-dBm normalization
- [x] circuit/packet/EPS registration and operator state
- [x] URC-driven updates plus registered/unregistered polling cadence
- [x] deterministic reinitialize -> functional reset -> hard-reset recovery policy
- [x] board VBUS cycle with explicit unsupported behavior when external VBUS is configured
- [x] host tests for status parsing and recovery policy

Acceptance: survive SIM removal/reinsertion, denied/searching/roaming transitions and repeated modem functional resets without ESP restart; recovery must not power-cycle a healthy modem after the escalation history has been cleared by stable polls.

## M4 - SMS

Implementation is present for durable text SMS; target-side compile and physical modem behavior remain gated by hosted CI/hardware.

- [x] PDU codec: GSM-7 and UCS-2/UTF-16 text; 8-bit binary detection with raw-PDU quarantine
- [x] multipart UDH segmentation/reassembly with 8-bit and 16-bit references
- [x] incoming `+CMTI`/`+CDSI`, direct `+CMT`/`+CDS`, and restart `CMGL` drain
- [x] durable outgoing segment queue with atomic `CMGS` prompt transactions
- [x] delivery reports and per-segment TP-MR correlation
- [x] complete-message and multipart-part duplicate suppression
- [x] bounded versioned NVS journal
- [x] conservative `uncertain` state for ambiguous interrupted sends
- [x] conservative automatic pruning of safe terminal records under storage pressure
- [ ] configurable age/count retention policy
- [ ] optional privileged binary-SMS API exposure

Acceptance: incoming text is committed before modem deletion; interrupted sends never auto-retry an ambiguous segment; multipart messages survive reboot between parts; a modem inbox larger than one AT response buffer is drained in batches without loss.

## M5 - Network/API

Implementation complete; target-side compile and physical network behavior remain gated by hosted CI/hardware.

- [x] secure SoftAP Wi-Fi provisioning and reconnect FSM
- [x] SNTP time synchronization
- [x] REST v1 handlers matching `api/openapi.yaml`
- [x] generated bearer-token authentication with digest-only persistence
- [x] persistent idempotency-key store
- [x] global/outbound-SMS rate limits and bounded request bodies
- [x] modem/SIM/reboot management routes without raw AT exposure

## M6 - MQTT / Home Assistant

Implementation complete; target-side compile and broker/Home Assistant integration remain gated by hosted CI/hardware.

- [x] versioned MQTT broker/TLS/authentication configuration
- [x] CA-verified `mqtts://`, LWT, retained availability/status and reconnect
- [x] bounded fragmented-payload reconstruction and worker-task isolation
- [x] persistent request idempotency for structured QoS-1 SMS sends
- [x] reject retained commands and require exact restart-button payloads
- [x] durable at-least-once incoming-SMS replay with persisted broker cursor
- [x] current MQTT Device Discovery document with required origin/device metadata
- [x] availability/registration/signal/operator/SIM/queue entities
- [x] incoming SMS event entity
- [x] restart modem/gateway buttons
- [x] optional default-recipient native notify entity
- [x] arbitrary-recipient example script using structured `mqtt.publish`
- [x] Recorder/SMS-content privacy guidance

## M7 - OTA and releases

Implementation complete; hosted target build and physical rollback fault injection remain pending.

- [x] authenticated streamed OTA upload endpoint
- [x] required whole-image SHA-256 integrity check
- [x] project/SemVer/reinstall/downgrade/secure-version policy
- [x] inactive-slot validation and boot selection through ESP-IDF OTA APIs
- [x] delayed rollback confirmation after critical service startup + stability window
- [x] OTA/status diagnostics in REST
- [x] tagged GitHub release workflow with embedded-version verification
- [x] OTA + merged factory binaries, release manifest and SHA-256 checksums
- [x] esptool initial/recovery flashing documentation
- [ ] physical crash/reset/power-loss rollback fault tests

Acceptance: a valid release OTA reboots into the new slot and is confirmed only after the stability window; an intentionally crashing image rolls back to the previous valid slot; corrupt/wrong-project/reinstall/downgrade images are rejected according to policy.

## M8 - Hardening / v1.0

- [ ] multi-day soak test
- [ ] Wi-Fi outage/recovery test
- [ ] MQTT outage/recovery test (durable replay implemented; hardware/broker soak pending)
- [ ] USB disconnect/reconnect test (CDC reprobe implemented; hardware soak pending)
- [ ] modem lockup/power-cycle test
- [ ] power-loss during queue writes
- [ ] fuzz AT/PDU parsers
- [ ] security review and threat model
