#!/usr/bin/env python3
"""Print the CHANGELOG.md section for one version (release notes).

Usage: changelog_section.py 0.7.0-alpha.14 [--changelog CHANGELOG.md]
Exits with status 1 when the changelog has no section for that version.
"""
from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, version: str) -> str | None:
    heading = re.compile(rf"^## {re.escape(version)}(\s+-\s+.*)?$")
    lines = text.splitlines()
    for start, line in enumerate(lines):
        if heading.match(line):
            end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
            body = "\n".join(lines[start + 1:end]).strip()
            return body or None
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("--changelog", type=Path, default=ROOT / "CHANGELOG.md")
    args = parser.parse_args()
    body = section(args.changelog.read_text(encoding="utf-8"), args.version.removeprefix("v"))
    if body is None:
        print(f"no CHANGELOG.md section for {args.version}", file=sys.stderr)
        return 1
    print(body)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
