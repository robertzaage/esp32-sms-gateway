# OTA, rollback, releases and flashing

M7 provides authenticated **push OTA** through the existing REST API and keeps
initial/recovery flashing deliberately simple with `esptool`. There is no browser
installer and no ESP Web Tools dependency.

## Image types

CI and tagged releases produce two primary binaries:

- `*-ota.bin` — the application image accepted by `POST /api/v1/system/firmware`.
- `*-factory.bin` — bootloader + partition table + application merged into one
  image for initial/recovery flashing at address `0x0`.

Never upload the merged factory image to the OTA endpoint. The endpoint accepts
only an ESP-IDF application image for the inactive OTA slot.

`release-manifest.json` contains the artifact names, sizes, SHA-256 digests,
target and ESP-IDF version. `SHA256SUMS` covers every release file.

## Initial or recovery flash with esptool

Install a current esptool release, put the board into download mode if automatic
reset does not do so, and identify the programming serial port.

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port PORT flash-id
```

Optional clean recovery:

```sh
python -m esptool --chip esp32s3 --port PORT erase-flash
```

Flash the merged image:

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x0 esp32-sms-gateway-v0.7.0-factory.bin
```

The merged image already contains the flash parameters produced by ESP-IDF. Do
not invent offsets for the OTA image; use the merged factory image for a fresh
device.

## OTA upload

The endpoint is authenticated with the same bearer token as the rest of the
management API. It accepts `application/octet-stream` and requires the SHA-256
of the exact OTA binary in `X-Firmware-SHA256`.

```sh
OTA=esp32-sms-gateway-v0.7.0-ota.bin
TOKEN='...'
GATEWAY='http://sms-gateway.local'
SHA256="$(sha256sum "$OTA" | awk '{print $1}')"

curl --fail-with-body \
  -X POST "$GATEWAY/api/v1/system/firmware" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/octet-stream' \
  -H "X-Firmware-SHA256: $SHA256" \
  -H 'Expect:' \
  --data-binary "@$OTA"
```

A successful response is `202 Accepted`. The gateway selects the inactive OTA
slot and reboots shortly after the response is sent.

The API rejects:

- an invalid/truncated ESP-IDF application image;
- a SHA-256 mismatch;
- an image whose project name is not `esp32_sms_gateway`;
- a non-SemVer application version;
- reinstalling the identical version unless explicitly allowed;
- a downgrade unless explicitly allowed;
- any image whose `secure_version` is lower than the running image;
- an image that does not fit the inactive OTA partition;
- another OTA while the current image is still pending rollback verification.

An intentional reinstall requires:

```text
X-Firmware-Allow-Reinstall: true
```

An intentional version downgrade requires:

```text
X-Firmware-Allow-Downgrade: true
```

These are separate safeguards. Neither header permits lowering ESP-IDF's
`secure_version` field.

## Rollback confirmation

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is enabled. On the first boot of a new
OTA image, ESP-IDF marks it `PENDING_VERIFY`.

The firmware does **not** confirm the image immediately. It first requires all
critical application services to initialize successfully, then waits
`CONFIG_GATEWAY_OTA_CONFIRM_DELAY_SECONDS` (30 seconds by default). Only after
that stability window does it call `esp_ota_mark_app_valid_cancel_rollback()`.

If the new image crashes, aborts, loses power or deliberately resets before it
is confirmed, the bootloader rolls back on the next boot to the previously
valid slot.

Inspect state with:

```sh
curl -H "Authorization: Bearer $TOKEN" \
  "$GATEWAY/api/v1/system/firmware"
```

OTA state is also included in `/api/v1/status`.

## Integrity versus authenticity

The required SHA-256 protects against accidental corruption and catches use of
the wrong release file. It is **not a substitute for transport or image
authenticity**: on plain HTTP an active network attacker could replace both the
image and checksum header.

Until native HTTPS or signed-app enforcement is enabled, perform OTA only on a
trusted management LAN or through a trusted TLS reverse proxy/VPN. Production
deployments with a stronger physical/network threat model should enable the
ESP32-S3 Secure Boot / signed-app / flash-encryption controls as appropriate.

## Tagged releases

Pushing a SemVer tag such as `v0.7.0` runs `.github/workflows/release.yml`.
Release CI sets the embedded application version from the tag, builds with the
pinned ESP-IDF version, parses the resulting `esp_app_desc_t` back out of the
binary to prove that project/version metadata matches, creates the OTA and
factory binaries, writes the manifest/checksums, and uploads them to the GitHub
Release.

The release workflow can also be manually dispatched for an **existing tag**;
it checks out that tag before building it.
