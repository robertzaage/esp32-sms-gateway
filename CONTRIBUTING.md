# Contributing

Contributions are welcome. The project is still being validated on real modem hardware, so small, testable changes are much easier to review than broad rewrites.

## Development setup

Use the ESP-IDF version pinned by the repository. Do not upgrade ESP-IDF or managed components as part of an unrelated change.

Before submitting firmware changes, run:

```sh
python scripts/validate_contracts.py
python -m unittest discover -s tests -v
idf.py set-target esp32s3
idf.py build
git diff --check
```

GitHub Actions runs the same host checks and a clean ESP-IDF build.

## What makes a good change

- Keep modem parsing, PDU handling and policy logic host-testable where practical.
- Add a regression test for parser, persistence, recovery and retry bugs.
- Keep USB callbacks and network callbacks lightweight; application work belongs in worker tasks.
- Preserve the single-owner AT model. New callers should use the existing modem/SMS services rather than writing AT commands directly.
- Treat ambiguous SMS submission as potentially delivered. Do not add automatic retries that can duplicate messages.
- Do not log SMS bodies, phone numbers, SIM PINs, bearer tokens, Wi-Fi passwords, MQTT passwords or private CA material at normal log levels.

## API and compatibility

`api/openapi.yaml` is the public REST contract. If an API change is intentional, update the implementation, OpenAPI file and human documentation together.

MQTT topics are also a public interface. Prefer additive changes and keep structured SMS commands idempotent through `request_id`.

## OTA changes

OTA code must remain rollback-safe. A new image is not marked valid until critical services have started and the configured stability period has elapsed. Changes to version or OTA policy should include host tests where possible.

The supported initial/recovery flashing path is the merged factory image plus `esptool`; there is no browser-flashing dependency.

## Commits

Use short, focused commit messages in the imperative style, for example:

```text
usb: handle modem re-enumeration
sms: preserve uncertain send state
api: document firmware upload errors
```
