# Hardware bring-up

This guide covers the reference combination: an **ESP32-S3-USB-OTG** board and a Huawei USB modem that eventually enumerates as `12d1:1506`.

## Connect the board

The board uses separate paths for programming/debug and USB host power:

1. Connect the Micro-USB debug/programming port to your computer.
2. Provide a regulated 5 V source to `USB_DEV` when using the default host-power configuration.
3. Plug the modem into the Type-A `USB_HOST` socket, directly or through a powered USB 2.0 hub.

The debug connector alone does not power the Type-A host socket.

The board's host path is current-limited to about 500 mA. Cellular transmit bursts can exceed that. If the modem resets, disappears, or repeatedly re-enumerates while registering on the network, suspect power before USB throughput.

## Serial console

Logs and flashing use the **Micro-USB** port (on-board USB-to-UART bridge, 115200 baud). The `USB_DEV` Type-A plug only supplies power. The ESP32-S3's native USB belongs to the modem port, so no USB serial device appears on `USB_DEV`.

```sh
python -m serial.tools.miniterm /dev/ttyUSB0 115200   # or: idf.py -p /dev/ttyUSB0 monitor
```

## Crash diagnostics and safe mode

The firmware keeps a core dump of the last crash in flash. At the next boot, a one-line summary (panic reason, task, PC and backtrace) is:

- shown on the display until a button is pressed;
- returned as `gateway.last_crash` in `/api/v1/status`;
- shown at the top of the setup portal page.

Decode the hex addresses with the ELF from the same release:

```sh
xtensa-esp32s3-elf-addr2line -pfiaC -e esp32-sms-gateway-vX.Y.Z.elf 42011a9f 420091b4
```

`gateway.reset_reason` says why the current boot started. For a deliberate software restart, `gateway.restart_cause` names its source: `firmware update installed`, `API /system/reboot`, `MQTT system/reboot command`, `Wi-Fi setup completed` or `USB mode-switch transfer stuck`. `not recorded` means a software restart that did not go through the gateway's restart path.

After 3 crash or brownout resets in a row, the gateway boots in **safe mode** without the USB modem stack. After 6, it also skips the display. Wi-Fi, the setup portal and the REST API keep running, so the crash can still be read. The counter clears after 60 seconds of stable operation or a power cycle.

## Flashing

For a GitHub release, use the merged factory image:

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port PORT flash-id
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x0 esp32-sms-gateway-vX.Y.Z-factory.bin
```

For development with ESP-IDF:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Use the board's programming/serial port, not a `/dev/ttyUSB*` node created by a modem connected to your computer.

## What should happen at boot

The firmware sets `USB_SEL` (GPIO18) high to route the ESP32-S3 USB peripheral to the `USB_HOST` socket, enables the current limiter and the configured VBUS path, starts the USB Host stack and waits for the modem.

Huawei modems can appear first as a storage device. The gateway recognizes common pre-switch IDs such as `12d1:1f01` (E3372), `12d1:1446` and `12d1:14fe`, sends the Huawei mode-switch command, and waits for the modem personality to re-enumerate. An additional source PID can be configured for hardware that uses a different cold-boot ID.

For the known `12d1:1506` descriptor, interface 1 is normally the best AT candidate. Firmware does not depend on that number: it ranks compatible interfaces from the live USB descriptor and keeps the first one that answers `AT` with `OK`.

A healthy boot should progress from USB discovery to an AT-ready modem, then SIM and cellular registration. `modem.state` in `/api/v1/status` then reads `ready`, and `sms.pdu_mode_configured` is `true` once SMS handling is set up.

Huawei modems send unsolicited results such as `+CMTI` (new SMS) and `+CDS` (delivery report) to their PC UI port by default. The gateway talks to the modem port, so it sets `AT^PORTSEL=1` during initialization and additionally polls the modem inbox every 60 seconds.

## Powered hubs

USB hub support is enabled. A powered USB 2.0 hub is recommended when the modem is unstable on the board's host supply.

A self-powered hub may still require upstream VBUS for attach/session detection, so do not disable board host VBUS simply because the hub has its own power supply. With a hub-powered modem, the board generally cannot remove power from the modem during recovery; functional modem reset remains available.

## Over-current behavior

The board's over-current input (GPIO21, the MIC2005A `FAULT/` output, low = fault) is polled every 100 ms. Faults shorter than about 300 ms, such as the inrush when a modem is plugged in, are ignored. When firmware owns host VBUS and a fault persists, it cuts host power and restores it 5 seconds after the fault has cleared. `modem.usb_overcurrent`, `usb_power_cutoff_latched` and the event counters in `/api/v1/status` show the state.

Repeated over-current recovery is a sign to fix the power topology, not a normal operating condition.

## Useful checks

If the modem never appears:

- confirm the modem LED/power state;
- verify the `USB_DEV` 5 V input;
- try a powered USB 2.0 hub;
- check `/api/v1/status` (`modem.usb_overcurrent`, `usb_power_cutoff_latched`, `usb_mode_switch_*`) and the serial log for over-current messages or an unsupported Huawei cold-boot PID.

If the modem is `ready` but SMS do not go out or come in, check `sms.pdu_mode_configured` and `sms.setup_failed` in `/api/v1/status`. `POST /api/v1/modem/at` (see [Networking and REST API](networking-api.md#modem-at-diagnostics)) runs single AT commands such as `AT+CPMS?`, `AT+CNMI?` or `AT^PORTSEL?` without a serial cable.

## Memory

The board has no PSRAM, so Wi-Fi, TLS, the USB host stack, the display and the modem services share about 340 KB of internal RAM. `gateway.heap` in `/api/v1/status` reports free RAM, the largest free block and the lowest free value since boot. With an open `mqtts://` session, expect roughly 60 KB free and a largest block around 30 KB. A low point near zero or a largest block under about 16 KB means TLS connections can fail.

If USB connects but no AT port is found, capture the complete USB descriptor and serial log. The modem may expose a different composite layout.

If an AT candidate opens but never returns `OK`, capture DEBUG logs before changing line coding or endpoints. Huawei vendor-specific modem ports often work without conventional UART-style baud configuration.

## Hardware acceptance checklist

Before unattended use, verify at least:

- cold boot with the modem already attached;
- plug-in after boot;
- repeated unplug/replug without rebooting the ESP32;
- SIM PIN handling if your SIM uses a PIN;
- home/roaming registration and signal reporting;
- send and receive SMS, including a Unicode and multipart message;
- powered-hub operation if one is required;
- modem restart/recovery;
- Wi-Fi and MQTT reconnect;
- OTA success and intentional rollback testing.
