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

## M7 release/OTA checks

- Keep `PROJECT_VER` valid Semantic Version syntax; development defaults to
  `0.7.0-dev` and tagged release CI overrides it from the `vMAJOR.MINOR.PATCH`
  tag.
- Run `python scripts/validate_contracts.py` and
  `python -m unittest discover -s tests -v` before pushing.
- Changes to OTA policy must add host tests in `tests/test_ota_version_policy.c`.
- Do not mark a pending OTA image valid earlier than the post-service stability
  window. Startup failure must remain rollback-safe.
- Release CI must continue to verify the `esp_app_desc_t` embedded in the built
  OTA binary before publishing it.
- The supported recovery path is `esptool` + the merged factory image; do not add
  a browser flashing dependency without an explicit project decision.
