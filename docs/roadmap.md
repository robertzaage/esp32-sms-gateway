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

- [ ] install USB Host service/task
- [ ] detect connect/disconnect without leaks
- [ ] identify `12d1:1506`
- [ ] enumerate candidate serial interfaces
- [ ] attempt `usb_host_cdc_acm` vendor-specific open
- [ ] fallback raw bulk transport when required
- [ ] dynamic `AT\r` probe and `OK` detection
- [ ] expose USB descriptor/selected interface in diagnostics
- [ ] tolerate repeated physical unplug/replug

Acceptance: 1,000 automated VBUS cycles without needing an ESP reboot, subject to modem power stability.

## M2 - AT engine

- [ ] single serialized command queue
- [ ] incremental CR/LF tokenizer
- [ ] response vs URC routing
- [ ] `OK`, `ERROR`, `+CME ERROR`, `+CMS ERROR`, prompt handling
- [ ] per-command deadlines/cancellation/retry policy
- [ ] random packet-fragmentation host tests
- [ ] interleaved URC tests

## M3 - Modem manager

- [ ] identification (`ATI`, manufacturer/model/IMEI)
- [ ] SIM/PIN state
- [ ] CSQ/RSSI normalization
- [ ] registration/operator state
- [ ] progressive recovery counters/reasons
- [ ] safe modem VBUS cycle

## M4 - SMS

- [ ] PDU codec: GSM-7, 8-bit, UCS-2
- [ ] multipart UDH segmentation/reassembly
- [ ] incoming `+CMTI` processing
- [ ] outgoing queue
- [ ] delivery reports
- [ ] duplicate suppression
- [ ] bounded persistent journal
- [ ] storage cleanup policy

## M5 - Network/API

- [ ] Wi-Fi provisioning and reconnect FSM
- [ ] time synchronization
- [ ] REST v1 handlers matching `api/openapi.yaml`
- [ ] bearer token authentication
- [ ] idempotency-key store
- [ ] rate limits and request size limits
- [ ] MQTT TLS/authentication/LWT

## M6 - Home Assistant

- [ ] MQTT Discovery device
- [ ] availability/registration/signal/operator entities
- [ ] incoming SMS event entity
- [ ] default-recipient/contact notify entities
- [ ] arbitrary-recipient example script using `mqtt.publish`
- [ ] recorder/privacy guidance

## M7 - OTA and releases

- [ ] authenticated OTA upload endpoint
- [ ] OTA integrity/version policy
- [ ] rollback fault tests
- [ ] GitHub tagged releases
- [ ] ESP Web Tools manifest/page

## M8 - Hardening / v1.0

- [ ] multi-day soak test
- [ ] Wi-Fi outage/recovery test
- [ ] MQTT outage/recovery test
- [ ] USB disconnect/reconnect test
- [ ] modem lockup/power-cycle test
- [ ] power-loss during queue writes
- [ ] fuzz AT/PDU parsers
- [ ] security review and threat model
