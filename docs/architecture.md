# Architecture

The gateway is built as a small appliance rather than a collection of independent AT-command endpoints. REST, MQTT and Home Assistant all feed the same modem and SMS services, which keeps retry and persistence behavior consistent.

## Main data path

```text
REST / MQTT / Home Assistant
            |
        SMS service
            |
       modem manager
            |
        AT engine
            |
       USB transport
            |
   Huawei USB modem
```

The board, Wi-Fi, settings, security and OTA services sit alongside that path and provide the surrounding appliance behavior.

## One owner of the modem

Only the AT engine writes application commands to the selected modem interface. Callers can submit work concurrently, but commands are serialized before they reach USB.

This matters most for prompt-based commands such as `AT+CMGS`: the command, `>` prompt, SMS PDU and final result are one atomic transaction. Another task cannot accidentally inject an AT command into the SMS body.

Unsolicited modem messages such as registration changes and incoming-SMS notifications are routed separately from command responses. USB callbacks only move bounded data into queues; they do not run application logic.

## Durable SMS state

Outgoing SMS is written to the dedicated NVS journal before the API reports it queued. Incoming SMS is persisted before the modem copy is deleted.

If a send is interrupted after the modem may have accepted a segment, the message becomes `uncertain`. The gateway deliberately refuses to guess whether it should resend, because guessing can create duplicate SMS. An operator can explicitly retry an uncertain message while acknowledging that risk.

The journal is bounded. When it fills, only safe terminal records are eligible for automatic pruning. Active, ambiguous, delivery-report-pending and unpublished incoming records remain protected.

## USB and modem recovery

The USB transport discovers compatible Huawei serial-like interfaces from the live descriptor and verifies the AT port with an `AT`/`OK` probe. Common Huawei mass-storage personalities can be switched before normal modem probing.

Recovery escalates gradually:

1. reapply the modem AT profile;
2. request a modem functional reset;
3. power-cycle board-owned host VBUS when the hardware topology allows it.

A powered hub can improve modem power stability, but it normally prevents the ESP32 from removing downstream modem power. In that case the gateway reports that a true hard reset is unavailable instead of pretending it succeeded.

## Networking

Wi-Fi provisioning and reconnect are handled outside the modem path. REST handlers authenticate and validate requests before calling services. MQTT callbacks reconstruct bounded messages and hand them to a worker task rather than touching the modem directly.

REST and structured MQTT SMS sends share the persistent idempotency ledger. Each ingress also applies its own rate limits.

## MQTT delivery

Incoming SMS is already durable before MQTT publication. The gateway publishes one incoming record at a time with QoS 1 and advances a persisted replay cursor only after the MQTT client reports publication acknowledgement. Reconnects can therefore repeat an event, but they do not create or lose the underlying SMS record.

## OTA

OTA is streamed directly into the inactive application partition; the complete image is not held in RAM. The upload is checked for project/version policy and SHA-256 integrity before it becomes the next boot partition.

A new image remains pending after its first boot. It is marked valid only after critical services initialize and the stability timer expires. A crash or reset before confirmation lets the ESP-IDF bootloader roll back to the previous valid image.

## Flash layout

The 8 MB flash is split between normal NVS/configuration, two 3 MB OTA application slots and a dedicated storage partition for the SMS journal. See `partitions.csv` for the exact layout.
