#!/usr/bin/env python3
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--ota", required=True)
    parser.add_argument("--factory", required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--output", default="release-manifest.json")
    args = parser.parse_args()

    ota = args.dist / args.ota
    factory = args.dist / args.factory
    for path in (ota, factory):
        if not path.is_file():
            raise SystemExit(f"missing release artifact: {path}")

    manifest = {
        "schema": 1,
        "project": "esp32_sms_gateway",
        "version": args.version,
        "target": "esp32s3",
        "esp_idf": "6.0.2",
        "source_revision": args.git_sha,
        "artifacts": {
            "ota": {
                "file": ota.name,
                "size": ota.stat().st_size,
                "sha256": sha256(ota),
                "api_content_type": "application/octet-stream",
                "api_sha256_header": "X-Firmware-SHA256",
            },
            "factory": {
                "file": factory.name,
                "size": factory.stat().st_size,
                "sha256": sha256(factory),
                "flash_offset": "0x0",
                "esptool_chip": "esp32s3",
            },
        },
    }
    out = args.dist / args.output
    out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
