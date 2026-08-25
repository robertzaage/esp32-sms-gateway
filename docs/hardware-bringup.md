# Hardware bring-up: Huawei `12d1:1506`

This document covers the first hardware acceptance test for the ESP32-S3-USB-OTG board.

## Connections

The board uses separate connectors for debug/programming and host VBUS.

1. Connect the **Micro-USB** debug connector to the development computer for flashing and serial logs.
2. Connect a regulated **5 V source** to the board's `USB_DEV` Type-A male connector when using the default `GATEWAY_USB_HOST_POWER_USB_DEV` setting.
3. Insert the Huawei modem into the **USB_HOST** Type-A female connector.

The Micro-USB debug connector does not provide VBUS to the Type-A host socket. The board's host current limiter is 500 mA; if the modem resets or disconnects during registration/transmit, put a powered USB 2.0 hub between the board and modem.

## Build and flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Use the serial port belonging to the board's USB-to-UART bridge. Exit `idf.py monitor` with `Ctrl-]`.

## Expected M1 behavior

At boot the firmware:

1. routes the ESP32-S3 native USB peripheral to the Type-A host connector;
2. enables the selected VBUS source and current limiter;
3. installs the ESP-IDF USB Host library and `usb_host_cdc_acm` driver;
4. waits for Huawei `12d1:1506`;
5. parses the active configuration descriptor;
6. ranks vendor-specific alternate-setting-0 interfaces that contain bulk IN and OUT endpoints;
7. opens candidates in priority order and sends `AT\r` up to three times;
8. keeps the first interface that returns a complete `OK` line.

For the descriptor captured during development, the expected ranking is:

```text
candidate[0]: if=1 proto=01 bulk-in=0x83 bulk-out=0x03 intr-in=0x84
candidate[1]: if=0 proto=12 bulk-in=0x82 bulk-out=0x02
```

The NCM network data alternate setting is deliberately excluded.

A successful boot should eventually contain log lines similar to:

```text
I (...) modem_usb: Huawei modem 12d1:1506 connected; 2 serial candidate(s)
I (...) modem_usb: opening Huawei interface 1 for AT probe
I (...) modem_usb: AT probe interface=1 attempt=1/3
I (...) modem_usb: AT probe succeeded on interface 1
I (...) modem_core: state=at_probe -> at_ready usb=12d1:1506 if=1 at=1
```

Endpoint maximum packet sizes may be 64 bytes when attached to ESP32-S3 at USB Full Speed even though Linux showed 512-byte High-Speed endpoints. The firmware intentionally uses the descriptors presented by the live connection rather than hardcoding packet size.

## Failure signatures

### No Huawei connection log

Check host VBUS first. If the modem LED stays off, verify the `USB_DEV` 5 V source or use a powered hub. Also check for:

```text
USB host over-current is asserted at boot
```

### Device connects but zero candidates are found

Capture the complete serial log. This means the USB personality differs from the descriptor used to build the initial parser, or the modem has not switched to `12d1:1506`.

### Interfaces open but no `OK`

The firmware will try all eligible candidates. Capture logs at DEBUG level before changing control-line or line-coding behavior; Huawei vendor-specific modem ports generally do not need conventional UART baud-rate setup.

### Repeated disconnects during cellular registration

Treat power as the first suspect. The ESP32-S3 USB transport is Full Speed and the modem descriptor explicitly reports Full Speed as fully functional, so throughput is not a concern for AT/SMS operation.

## Acceptance before M2

M1 is considered hardware-verified when all of the following hold:

- cold boot with the modem already attached reaches `at_ready`;
- attaching the modem after boot reaches `at_ready`;
- unplug/replug returns from `wait_usb` to `at_ready` without rebooting the ESP32;
- at least 20 manual reconnect cycles succeed before starting automated VBUS-cycle testing;
- no over-current events or modem brownouts occur in the chosen power topology.

## Huawei cold-boot personalities

The gateway also listens for common Huawei pre-switch mass-storage personalities `12d1:1446` and `12d1:14fe`. When one is seen, a dedicated USB Host client claims a mass-storage bulk-OUT interface and sends the standard Huawei switch CBW, then waits for the device to disappear and re-enumerate as the configured modem PID (`12d1:1506` by default). One additional pre-switch PID can be configured with `GATEWAY_MODEM_USB_SWITCH_PID_EXTRA`.

The mode-switch client is registered before the CDC client so a modem already attached during cold boot is less likely to race enumeration. Mode-switch attempts/successes/failures are exposed in `/api/v1/status`.

## Powered USB hubs

`CONFIG_USB_HOST_HUBS_SUPPORTED=y` is enabled. A powered USB 2.0 hub is the recommended fallback if the modem exceeds the board's 500 mA host current limit. Keep in mind that board GPIO VBUS control cannot necessarily remove power from a hub's downstream modem port; in that topology software functional reset remains the hard-reset fallback unless the hub itself provides switchable downstream power.

The board continuously monitors its over-current input. When firmware owns host VBUS, an asserted fault immediately cuts VBUS and latches the condition. Power is restored only after the signal has remained clear for the cooldown interval. Over-current events and cutoffs are visible in `/api/v1/status`.

## Flashing release artifacts without ESP-IDF

For initial/recovery tests you can use the release's merged factory image with
`esptool`; ESP-IDF does not need to be installed on the flashing computer:

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x0 esp32-sms-gateway-vX.Y.Z-factory.bin
```

Use the separate `*-ota.bin` only with the authenticated firmware endpoint.
See [ota-releases.md](ota-releases.md) for checksum, version-policy and rollback
verification instructions.
