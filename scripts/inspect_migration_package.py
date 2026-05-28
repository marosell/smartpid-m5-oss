#!/usr/bin/env python3
"""Inspect a ProofPro OEM-layout migration package."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


PACKAGE_MAGIC = b"PPMIG001"
PACKAGE_HEADER_SIZE = 12


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect_package(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < PACKAGE_HEADER_SIZE:
        raise SystemExit(f"{path} is too small to be a migration package")
    magic = data[:8]
    if magic != PACKAGE_MAGIC:
        raise SystemExit(f"{path} has bad magic {magic!r}")
    manifest_len = struct.unpack("<I", data[8:12])[0]
    manifest_start = PACKAGE_HEADER_SIZE
    manifest_end = manifest_start + manifest_len
    if manifest_end > len(data):
        raise SystemExit(f"{path} manifest length exceeds package size")

    manifest = json.loads(data[manifest_start:manifest_end].decode("utf-8"))
    cursor = manifest_end
    artifacts = []
    errors = []
    for item in manifest.get("artifacts", []):
        size = int(item["size"])
        blob = data[cursor : cursor + size]
        actual_sha = sha256_bytes(blob)
        ok = len(blob) == size and actual_sha == item["sha256"]
        if not ok:
            errors.append(f"{item['role']} payload mismatch")
        artifacts.append(
            {
                "role": item["role"],
                "offset": item["offset"],
                "offset_hex": item["offset_hex"],
                "size": size,
                "payload_offset": cursor,
                "payload_offset_hex": f"0x{cursor:x}",
                "sha256": actual_sha,
                "ok": ok,
            }
        )
        cursor += size

    if cursor != len(data):
        errors.append(f"package has {len(data) - cursor} trailing bytes")

    return {
        "path": str(path),
        "size": len(data),
        "sha256": sha256_bytes(data),
        "manifest_len": manifest_len,
        "schema": manifest.get("schema"),
        "schema_version": manifest.get("schema_version"),
        "boot_app": manifest.get("boot_app"),
        "boot_slot": manifest.get("boot_slot"),
        "safe_to_flash": manifest.get("safe_to_flash"),
        "artifacts": artifacts,
        "valid": not errors,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect a ProofPro migration package.")
    parser.add_argument("path", type=Path)
    args = parser.parse_args()

    result = inspect_package(args.path)
    print(json.dumps(result, indent=2))
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
