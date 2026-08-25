# Modem manager (M3)

The modem manager turns a working AT transport into an unattended modem service. It is the only component that owns long-lived modem policy: initialization, SIM state, registration, signal/operator status, health polling, and recovery escalation.

## Ownership and events

`modem_manager` runs one FreeRTOS task. AT-engine URCs and USB transport changes are copied into a bounded queue; they do not mutate modem state from callback context. This keeps polling, initialization and asynchronous registration changes serialized.

The manager consumes the M2 `at_engine` through an injected execute callback. It never writes USB directly.

## Initialization profile

After M1 reports an AT-ready interface, M3 performs:

1. `ATE0` - disable local echo.
2. `AT+CMEE=2` - verbose modem errors.
3. `AT+CGMI`, `AT+CGMM`, `AT+CGMR` - manufacturer/model/revision.
4. `AT+CGSN` - IMEI for local diagnostics only.
5. `AT+CPIN?` - SIM state.
6. `AT+CREG=2`, `AT+CGREG=2`, `AT+CEREG=2` with fallback to mode 1 where supported.
7. registration queries, `AT+CSQ` and `AT+COPS?` when the SIM is ready.

Unsupported packet/EPS registration commands are optional. This is important for older Huawei 2G/3G firmware.

The IMEI is kept in the internal snapshot but must not be published through Home Assistant discovery, MQTT status, or normal INFO logs by default. Diagnostics also retain only structured AT result/error codes and the last recovery action; command text is not logged by M3.

## SIM handling

Supported states include ready, PIN required, PUK required, absent, blocked and unknown/error. A PIN can be submitted to the internal manager API only when the modem reports `SIM PIN`.

PIN rules:

- 4-8 decimal digits.
- never logged;
- never persisted in M3;
- copied into a bounded queue item;
- command buffer is zeroized after use.

Persistent SIM PIN storage, if added later, must use the same secret-storage policy as API/MQTT credentials rather than plain NVS strings.

## Network state

M3 tracks circuit (`+CREG`), packet (`+CGREG`) and EPS (`+CEREG`) registration independently. Overall `registered=true` when any supported domain reports home or roaming service. EPS is preferred over packet and circuit state when deriving roaming status.

`+CSQ` is normalized to dBm using the 3GPP mapping `-113 + 2*rssi` for values 0-31. RSSI 99 remains unknown rather than inventing a signal value.

`AT+COPS?` is deliberately polled much less frequently than registration/signal because operator selection queries can be comparatively slow on real modems.

## Polling

Defaults are configurable in Kconfig:

- 5 s while waiting for SIM/network.
- 30 s while registered.
- 5 min operator refresh.

URCs update state immediately, so polling is a health/reconciliation mechanism rather than the only source of truth. Registration changes also schedule an immediate reconciliation so operator/signal state catches up quickly after a network transition.

## Recovery ladder

AT failures do not immediately power-cycle hardware. Once the configured consecutive-failure threshold is reached, recovery escalates deterministically:

1. Re-run the safe AT initialization profile.
2. Request `AT+CFUN=1,1` and allow the modem to re-enumerate.
3. Power-cycle Type-A VBUS through the ESP32-S3-USB-OTG board.

Three consecutive healthy status poll cycles reset the escalation history.

The board abstraction reports whether firmware owns a Type-A VBUS source. When configured with `GATEWAY_USB_HOST_POWER_OFF` (for example because a powered hub supplies the modem), firmware does not pretend it can hard-reset the modem; it enters `degraded` and reports `ESP_ERR_NOT_SUPPORTED` instead.

An ESP32 reboot is intentionally not part of the normal M3 recovery ladder. The task watchdog and later system supervisor cover firmware failures; modem faults should be isolated to the modem whenever possible.

## Host-testable logic

`modem_status.c` and `modem_recovery_policy.c` are ESP-IDF-independent. CI compiles them with the host compiler and verifies:

- SIM state parsing;
- CSQ validation/dBm conversion;
- query and URC registration forms;
- operator parsing;
- deterministic recovery escalation.

Physical acceptance still requires the target board and modem.
