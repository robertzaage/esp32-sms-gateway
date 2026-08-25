# Architecture

## Design principles

1. **Single modem owner.** Only one task may write AT commands. REST, MQTT and internal services enqueue requests.
2. **Explicit state.** USB/modem recovery is an observable finite-state machine with reasons and counters.
3. **Durability before acknowledgement.** An outbound SMS is persisted before the API reports it queued; an inbound SMS is persisted before it is deleted from modem storage.
4. **At-least-once transports, idempotent application.** MQTT QoS 1 and HTTP retries are expected. Message IDs/idempotency keys prevent accidental duplicate sends.
5. **Dependency inversion.** SMS/PDU and AT parsing remain independent of USB and networking so they are host-testable.
6. **Bounded resources.** Queues, storage and logs have explicit limits and backpressure behavior.
7. **Secure defaults.** No raw AT endpoint, secret logging or anonymous management API in production mode.

## Layering

```text
REST / MQTT / Home Assistant / Webhooks
                 |
          Application services
      SMS | Status | USSD | Diagnostics
                 |
             Modem core
          composition / API
                 |
           Modem manager
     SIM | network | health | recovery
                 |
              AT engine
                 |
        Modem transport interface
          |                 |
   CDC-like Huawei       raw USB fallback
          \                 /
             ESP USB Host
                 |
       ESP32-S3-USB-OTG board
```

## Modem lifecycle

```text
BOOT -> WAIT_USB -> ENUMERATE -> FIND_AT_INTERFACE -> AT_PROBE -> AT_READY
  -> SIM_CHECK -> NETWORK_REGISTER -> READY
                                  \-> DEGRADED -> RECOVERY -> WAIT_USB
```

Recovery is progressive: command-level retry -> AT profile reinitialization -> modem functional reset (`AT+CFUN=1,1`) -> board-controlled VBUS power cycle. An ESP restart is not normal modem recovery; a later system supervisor/watchdog handles firmware-level failures separately.

## Huawei target

Known initial target:

- VID/PID: `12d1:1506`
- Composite device
- Current Linux observation exposes two USB serial functions
- Strong initial AT candidate: interface 1, bulk IN `0x83`, bulk OUT `0x03`, interrupt IN `0x84`

Firmware does not permanently hardcode interface 1. The M1 transport parses the active configuration descriptor, considers only vendor-specific alternate-setting-0 interfaces with bulk IN and OUT endpoints, ranks Huawei protocol `0x01` first and `0x12` second, and proves the selected interface by receiving a complete `OK` line for an `AT` probe. The NCM data alternate setting is excluded by construction.

## Concurrency model

Current/planned FreeRTOS ownership:

- `at_worker`: exclusive application-layer AT writer; owns command transaction lifecycle and incremental parser.
- USB callback/context: copies bytes into a bounded RX queue only; no response parsing/business logic.
- `at_urc`: dispatches unsolicited lines outside the transaction worker so consumers cannot block parsing.
- `modem_manager`: single writer for SIM/network/operator/signal state, periodic reconciliation and recovery policy. URCs are queued into this task before mutating state.
- `modem_core`: composition root for USB transport, AT engine and modem manager; maps detailed manager state to the public lifecycle.
- `sms_service`: durable incoming/outgoing state, PDU handling and delivery-report correlation.
- REST tasks (M5) and MQTT worker (M6): validate requests and enqueue service commands; never touch the modem directly. MQTT callbacks only reconstruct bounded packets and enqueue work.

Prompt-based commands remain one transaction: command -> prompt -> payload -> final result. No other caller can acquire modem ownership between the prompt and payload. Multi-line SMS/delivery-report URCs retain continuation state so payload text such as `OK` cannot be misclassified as another command's final result.

## Persistence

8 MB flash layout currently reserves:

- NVS: configuration/secrets metadata/counters, including versioned MQTT settings and persistent REST/MQTT idempotency records.
- dual 3 MB OTA slots.
- ~1.875 MB `storage` partition formatted as a dedicated NVS data partition for the durable SMS journal.

SMS records use an explicitly versioned fixed-width on-flash representation. The store is bounded and fails closed when full; it never erases itself automatically on an initialization/format error.

## OTA

The bootloader uses ESP-IDF rollback. A newly installed image remains pending until minimum boot self-tests succeed, then calls `esp_ota_mark_app_valid_cancel_rollback()`.


## MQTT / Home Assistant

MQTT runtime state is isolated behind `mqtt_service`. Broker callbacks never call SMS/modem APIs. Fragmented inbound MQTT payloads are reconstructed into bounded buffers and posted to a worker task. REST and structured MQTT SMS sends share the same persistent idempotency ledger and outbound token bucket.

Broker settings are stored as a versioned NVS record. `mqtts://` verifies the broker with either a configured private CA or Espressif's certificate bundle. Passwords and CA contents are write-only through REST and are excluded from normal logs.

Home Assistant uses one retained MQTT Device Discovery document with mandatory device/origin metadata and multiple components. SMS events are non-retained. The native notify entity is intentionally QoS 0 because Home Assistant's generic notify command does not provide an application idempotency key; critical/arbitrary-recipient automations use the structured QoS-1 send topic instead.

## M6.1 reliability boundaries

The MQTT inbound-SMS path is backed by the same durable SMS journal as REST. A persisted broker cursor is a retention watermark: incoming records newer than the cursor are not eligible for storage-pressure pruning while MQTT is enabled. This intentionally yields at-least-once broker events and exactly-one durable SMS record.

USB recovery treats a fatal CDC callback as transport loss. The selected handle is closed and candidate probing restarts; per-open generations prevent late callbacks from a retired handle from affecting the replacement session. Huawei mass-storage mode switching is performed by a separate raw USB Host client and remains isolated from the CDC/AT transport.

## M7 OTA service

OTA is a separate service rather than an HTTP-handler implementation detail.
The API server owns authentication, rate limiting and HTTP streaming; the OTA
service owns image prefix/app-descriptor policy, streaming SHA-256, inactive
partition writes, ESP-IDF validation, boot-partition selection and delayed
rollback confirmation. The OTA image is never buffered wholesale in RAM.

A new image remains pending after boot until every critical service in
`app_main` has initialized and the stability timer has elapsed. This makes a
startup crash or reset an automatic rollback event rather than a permanently
selected broken image.
