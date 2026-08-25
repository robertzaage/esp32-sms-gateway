#!/usr/bin/env python3
"""Inspect the fixed esp_app_desc_t embedded near the start of an ESP-IDF app image."""

from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct
import sys

ESP_IMAGE_HEADER_SIZE = 24
ESP_IMAGE_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_IMAGE_SEGMENT_HEADER_SIZE
ESP_APP_DESC_SIZE = 256
ESP_APP_DESC_MAGIC = 0xABCD5432


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def inspect(path: Path) -> dict[str, object]:
    raw = path.read_bytes()
    if len(raw) < ESP_APP_DESC_OFFSET + ESP_APP_DESC_SIZE:
        raise ValueError("file is too small to contain esp_app_desc_t")
    desc = raw[ESP_APP_DESC_OFFSET : ESP_APP_DESC_OFFSET + ESP_APP_DESC_SIZE]
    magic, secure_version = struct.unpack_from("<II", desc, 0)
    if magic != ESP_APP_DESC_MAGIC:
        raise ValueError(f"unexpected esp_app_desc_t magic 0x{magic:08x}")
    return {
        "file": path.name,
        "size": len(raw),
        "secure_version": secure_version,
        "version": cstr(desc[16:48]),
        "project_name": cstr(desc[48:80]),
        "build_time": cstr(desc[80:96]),
        "build_date": cstr(desc[96:112]),
        "idf_version": cstr(desc[112:144]),
        "elf_sha256": desc[144:176].hex(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--expect-project")
    parser.add_argument("--expect-version")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        info = inspect(args.image)
    except (OSError, ValueError, UnicodeDecodeError) as exc:
        print(f"image inspection failed: {exc}", file=sys.stderr)
        return 1
    if args.expect_project and info["project_name"] != args.expect_project:
        print(f"project mismatch: {info['project_name']!r} != {args.expect_project!r}", file=sys.stderr)
        return 2
    if args.expect_version and info["version"] != args.expect_version:
        print(f"version mismatch: {info['version']!r} != {args.expect_version!r}", file=sys.stderr)
        return 3
    if args.json:
        print(json.dumps(info, indent=2, sort_keys=True))
    else:
        print(f"project={info['project_name']} version={info['version']} idf={info['idf_version']} size={info['size']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
