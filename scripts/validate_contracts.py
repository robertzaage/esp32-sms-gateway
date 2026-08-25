#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"contract validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    text = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")
    required_fragments = [
        "openapi: 3.1.0",
        "version: 0.7.0",
        "/api/v1/health:",
        "/api/v1/status:",
        "/api/v1/messages:",
        "Idempotency-Key",
        '"202":',
        '"422":',
        "/api/v1/system/idempotency/clear-pending:",
        "/api/v1/system/firmware:",
        "X-Firmware-SHA256",
        "additionalProperties: false",
        "application/problem+json",
    ]
    for fragment in required_fragments:
        if fragment not in text:
            fail(f"missing required OpenAPI contract fragment: {fragment}")

    if not re.search(r"pattern:\s+['\"]?\^\\\\\+", text):
        fail("E.164 validation pattern is missing")

    print("contracts OK")


if __name__ == "__main__":
    main()
