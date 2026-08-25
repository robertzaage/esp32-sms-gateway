# Hardware bring-up

This guide covers the reference combination: an **ESP32-S3-USB-OTG** board and a Huawei USB modem that eventually enumerates as `12d1:1506`.

## Connect the board

The board uses separate paths for programming/debug and USB host power:

1. Connect the Micro-USB debug/programming port to your computer.
2. Provide a regulated 5 V source to `USB_DEV` when using the default host-power configuration.
3. Plug the modem into the Type-A `USB_HOST` socket, directly or through a powered USB 2.0 hub.

The debug connector alone does not power the Type-A host socket.

The board's host path is current-limited to about 500 mA. Cellular transmit bursts can exceed that. If the modem resets, disappears, or repeatedly re-enumerates while registering on the network, suspect power before USB throughput.

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

The firmware routes the ESP32-S3 USB peripheral to the host connector, enables the configured VBUS path, starts the USB Host stack and waits for the modem.

Huawei modems can appear first as a storage device. The gateway recognizes common pre-switch IDs such as `12d1:1446` and `12d1:14fe`, sends the Huawei mode-switch command, and waits for the modem personality to re-enumerate. An additional source PID can be configured for hardware that uses a different cold-boot ID.

For the known `12d1:1506` descriptor, interface 1 is normally the best AT candidate. Firmware does not depend on that number: it ranks compatible interfaces from the live USB descriptor and keeps the first one that answers `AT` with `OK`.

A healthy boot should progress from USB discovery to an AT-ready modem, then SIM and cellular registration.

## Powered hubs

USB hub support is enabled. A powered USB 2.0 hub is recommended when the modem is unstable on the board's host supply.

A self-powered hub may still require upstream VBUS for attach/session detection, so do not disable board host VBUS simply because the hub has its own power supply. With a hub-powered modem, the board generally cannot remove power from the modem during recovery; functional modem reset remains available.

## Over-current behavior

The board's over-current input is monitored while the gateway is running. When firmware owns host VBUS and a persistent fault is detected, it cuts host power, waits for the configured recovery interval, and then attempts to restore it. Counters are visible in `/api/v1/status`.

Repeated over-current recovery is a sign to fix the power topology, not a normal operating condition.

## Useful checks

If the modem never appears:

- confirm the modem LED/power state;
- verify the `USB_DEV` 5 V input;
- try a powered USB 2.0 hub;
- check the serial log for over-current messages or an unsupported Huawei cold-boot PID.

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
