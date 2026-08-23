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
       FSM | scheduler | URC routing
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
BOOT -> WAIT_USB -> ENUMERATE -> FIND_AT_INTERFACE -> AT_PROBE
  -> SIM_CHECK -> NETWORK_REGISTER -> READY
                                  \-> DEGRADED -> RECOVERY -> WAIT_USB
```

Recovery is progressive: command retry -> AT session reset -> USB interface reopen -> VBUS power cycle -> ESP restart only as a final fallback.

## Huawei target

Known initial target:

- VID/PID: `12d1:1506`
- Composite device
- Current Linux observation exposes two USB serial functions
- Strong initial AT candidate: interface 1, bulk IN `0x83`, bulk OUT `0x03`, interrupt IN `0x84`

Firmware must not permanently hardcode interface 1. It will enumerate plausible vendor-specific bulk IN/OUT interfaces and prove them by receiving `OK` for an `AT` probe.

## Concurrency model

Planned FreeRTOS ownership:

- `modem_task`: exclusive AT writer; owns command transaction lifecycle.
- USB callback/context: feeds bytes into a bounded receive buffer only.
- parser/dispatcher: converts byte stream into responses and unsolicited events.
- `sms_service`: durable incoming/outgoing state and PDU handling.
- API/MQTT tasks: validate requests and enqueue service commands; never touch the modem directly.

## Persistence

8 MB flash layout currently reserves:

- NVS: configuration/secrets metadata/counters.
- dual 3 MB OTA slots.
- ~1.875 MB `storage` partition for durable SMS journal/queue and diagnostics.

The filesystem implementation is intentionally not locked in by the first commit; its public persistence interface will be chosen before SMS storage lands.

## OTA

The bootloader uses ESP-IDF rollback. A newly installed image remains pending until minimum boot self-tests succeed, then calls `esp_ota_mark_app_valid_cancel_rollback()`.
