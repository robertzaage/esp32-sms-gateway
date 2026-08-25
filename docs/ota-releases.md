# OTA, releases and flashing

There are two firmware images to know about:

- `*-factory.bin` is a merged bootloader + partition table + application image for a fresh or recovery flash with `esptool`.
- `*-ota.bin` is the application image accepted by the gateway's OTA API.

Do not upload the factory image to the OTA endpoint.

GitHub release builds also include `release-manifest.json` and `SHA256SUMS` so you can verify artifact names, sizes, source revision and hashes.

## Initial or recovery flash

Install `esptool` and confirm that the board is visible:

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port PORT flash-id
```

For a clean recovery you may erase flash first:

```sh
python -m esptool --chip esp32s3 --port PORT erase-flash
```

Flash the merged factory image at `0x0`:

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x0 esp32-sms-gateway-vX.Y.Z-factory.bin
```

You do not need ESP-IDF installed on the flashing computer when you use the merged release image.

## OTA update

The OTA endpoint uses the same bearer token as the rest of the REST API. It expects the raw application image and the SHA-256 of that exact file.

```sh
OTA=esp32-sms-gateway-vX.Y.Z-ota.bin
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

A successful upload returns `202 Accepted`, selects the inactive OTA slot and reboots shortly afterward.

The gateway rejects corrupted/truncated images, a SHA-256 mismatch, the wrong project, invalid versions, images that do not fit the OTA slot and attempts to install a lower `secure_version`.

Reinstalling the same application version requires:

```text
X-Firmware-Allow-Reinstall: true
```

Installing an older application version requires:

```text
X-Firmware-Allow-Downgrade: true
```

These are deliberate operator overrides. Neither one bypasses ESP-IDF's `secure_version` anti-rollback check.

## Rollback behavior

ESP-IDF application rollback is enabled. A newly installed image boots in a pending-verification state.

The gateway does not mark it valid immediately. Critical services must initialize successfully and the firmware must remain alive for the configured stability window (30 seconds by default). Only then is the image confirmed.

If the new image crashes or resets before confirmation, the bootloader can return to the previous valid OTA slot on the next boot.

Check the current state with:

```sh
curl \
  -H "Authorization: Bearer $TOKEN" \
  "$GATEWAY/api/v1/system/firmware"
```

OTA state is also included in `/api/v1/status`.

## Security boundary

The required SHA-256 catches corruption and accidental use of the wrong file. It does **not** authenticate an upload made over plain HTTP; an active network attacker could replace both the binary and checksum header.

Perform OTA only on a trusted management network or through a trusted VPN/TLS reverse proxy. Deployments that need cryptographic firmware authenticity or protection against physical flash access should use the ESP32-S3 Secure Boot, signed-image and flash-encryption features appropriate to their threat model.

## Creating a release

Tags matching `v*.*.*` run `.github/workflows/release.yml`. The workflow reruns host tests, builds with ESP-IDF 6.0.2, embeds the version from the tag, verifies the version in the resulting application image, generates factory/OTA artifacts and publishes them to the GitHub Release.

For hardware validation builds, use a SemVer prerelease tag such as:

```sh
git tag -a v0.7.0-alpha.1 -m "ESP32 SMS Gateway v0.7.0-alpha.1"
git push origin v0.7.0-alpha.1
```

Keep prerelease tags for builds that have passed CI but still need physical board/modem validation.
