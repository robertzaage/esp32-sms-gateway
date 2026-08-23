# Contributing

## Build expectations

- ESP-IDF version is pinned; do not silently upgrade it in unrelated changes.
- Run `idf.py set-target esp32s3 && idf.py build` before merging firmware changes.
- Keep modem parsing/PDU logic transport-independent and host-testable.
- Add regression tests for parser, PDU, persistence or recovery bugs.
- Do not log SMS bodies, phone numbers or secrets at normal log levels.

## Commit style

Prefer focused imperative commits, for example:

- `usb: probe Huawei vendor-specific AT interfaces`
- `at: handle interleaved CMTI URCs`
- `ci: publish merged factory image`
