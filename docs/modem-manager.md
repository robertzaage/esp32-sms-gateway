# Modem management

The modem manager turns a working AT connection into a service that can run unattended. It owns SIM state, cellular registration, signal/operator status, periodic health checks and recovery policy.

## Initialization

After the USB layer finds a working AT port, the manager applies a conservative modem profile. It disables command echo, enables verbose modem errors, reads basic identity information, checks the SIM, enables registration notifications and starts registration/signal polling.

Older Huawei firmware does not support every packet or EPS registration command. Unsupported optional commands are tolerated rather than treated as a broken modem.

The IMEI is kept for local diagnostics but is not published through normal MQTT/Home Assistant status and should not appear in normal logs.

## SIM handling

The manager distinguishes common SIM states such as ready, PIN required, PUK required, absent and blocked.

A submitted SIM PIN must be 4–8 decimal digits. It is treated as a volatile secret: the value is not logged or persisted by the modem manager, and temporary command buffers are wiped after use.

## Registration and signal

Circuit, packet and EPS registration are tracked independently because modem/network combinations vary. The gateway considers the modem registered when any supported domain reports home or roaming service.

`AT+CSQ` is converted to dBm for normal values; RSSI 99 remains unknown rather than being turned into a made-up signal level. Operator information is refreshed less often than registration and signal because operator queries can be slow on real modems.

URCs update state promptly, while periodic polling acts as reconciliation in case a notification was lost.

## Recovery

A few failed AT commands do not immediately cut modem power. Recovery escalates only after repeated failures:

1. reapply the safe modem profile;
2. request `AT+CFUN=1,1` and allow the modem to restart/re-enumerate;
3. power-cycle Type-A VBUS if the board actually owns the modem's power path.

Healthy polling resets the escalation history.

When a powered hub supplies the modem, the board normally cannot perform a true power cycle. The gateway reports that limitation and remains degraded rather than claiming a hard reset occurred.

The ESP32 itself is not rebooted as ordinary modem recovery. Modem faults should stay isolated to the modem whenever possible.

## Diagnostics

Current modem, SIM, registration, signal, operator, USB reconnect and recovery information is exposed through `/api/v1/status` and the corresponding MQTT/Home Assistant status where appropriate.
