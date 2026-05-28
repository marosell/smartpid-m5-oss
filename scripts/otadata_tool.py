#!/usr/bin/env python3
"""Inspect and generate ESP32 OTA select data."""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path


ENTRY_SIZE = 32
SECTOR_SIZE = 0x1000
OTADATA_SIZE = 0x2000
UINT32_MAX = 0xFFFFFFFF

ESP_OTA_IMG_NEW = 0
ESP_OTA_IMG_PENDING_VERIFY = 1
ESP_OTA_IMG_VALID = 2
ESP_OTA_IMG_INVALID = 3
ESP_OTA_IMG_ABORTED = 4
ESP_OTA_IMG_UNDEFINED = UINT32_MAX

STATE_NAMES = {
    ESP_OTA_IMG_NEW: "new",
    ESP_OTA_IMG_PENDING_VERIFY: "pending_verify",
    ESP_OTA_IMG_VALID: "valid",
    ESP_OTA_IMG_INVALID: "invalid",
    ESP_OTA_IMG_ABORTED: "aborted",
    ESP_OTA_IMG_UNDEFINED: "undefined",
}


def ota_select_crc(ota_seq: int) -> int:
    """Match bootloader_common_ota_select_crc(): crc32_le(UINT32_MAX, ota_seq)."""
    return zlib.crc32(struct.pack("<I", ota_seq), UINT32_MAX) & UINT32_MAX


def make_entry(ota_seq: int, ota_state: int = ESP_OTA_IMG_UNDEFINED) -> bytes:
    return struct.pack("<I20sII", ota_seq, b"\xff" * 20, ota_state, ota_select_crc(ota_seq))


def make_otadata(boot_slot: str) -> bytes:
    if boot_slot == "app0":
        ota_seq = 1
    elif boot_slot == "app1":
        ota_seq = 2
    else:
        raise ValueError(f"unsupported boot slot: {boot_slot}")

    sector0 = make_entry(ota_seq) + (b"\xff" * (SECTOR_SIZE - ENTRY_SIZE))
    sector1 = b"\xff" * SECTOR_SIZE
    return sector0 + sector1


def parse_entry(data: bytes, index: int, app_count: int = 2) -> dict:
    if len(data) < ENTRY_SIZE:
        raise ValueError("entry data is too short")
    ota_seq, seq_label, ota_state, crc = struct.unpack("<I20sII", data[:ENTRY_SIZE])
    expected_crc = ota_select_crc(ota_seq) if ota_seq != UINT32_MAX else UINT32_MAX
    valid = (
        ota_seq != UINT32_MAX
        and ota_state not in (ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED)
        and crc == ota_select_crc(ota_seq)
    )
    selected_slot = None
    if valid and app_count > 0:
        selected_slot = f"app{(ota_seq - 1) % app_count}"
    return {
        "entry": index,
        "ota_seq": ota_seq,
        "ota_seq_hex": f"0x{ota_seq:08x}",
        "seq_label_hex": seq_label.hex(),
        "ota_state": ota_state,
        "ota_state_hex": f"0x{ota_state:08x}",
        "ota_state_name": STATE_NAMES.get(ota_state, "unknown"),
        "crc": crc,
        "crc_hex": f"0x{crc:08x}",
        "expected_crc": expected_crc,
        "expected_crc_hex": f"0x{expected_crc:08x}",
        "crc_ok": crc == expected_crc,
        "valid": valid,
        "selected_slot": selected_slot,
    }


def inspect_otadata(path: Path, offset: int = 0) -> dict:
    data = path.read_bytes()
    if len(data) < offset + OTADATA_SIZE:
        raise SystemExit(
            f"{path} is {len(data)} bytes; expected at least {offset + OTADATA_SIZE}"
        )
    data = data[offset : offset + OTADATA_SIZE]
    entries = [
        parse_entry(data[:ENTRY_SIZE], 0),
        parse_entry(data[SECTOR_SIZE : SECTOR_SIZE + ENTRY_SIZE], 1),
    ]
    valid_entries = [entry for entry in entries if entry["valid"]]
    active = max(valid_entries, key=lambda entry: entry["ota_seq"]) if valid_entries else None
    return {
        "path": str(path),
        "offset": offset,
        "offset_hex": f"0x{offset:x}",
        "size": len(data),
        "entries": entries,
        "active_entry": None if active is None else active["entry"],
        "active_slot": None if active is None else active["selected_slot"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect or generate ESP32 otadata.")
    sub = parser.add_subparsers(dest="command", required=True)

    inspect = sub.add_parser("inspect", help="Inspect an 8 KB otadata image")
    inspect.add_argument("path", type=Path)
    inspect.add_argument(
        "--offset",
        type=lambda value: int(value, 0),
        default=0,
        help="Byte offset to inspect inside a larger flash dump.",
    )

    generate = sub.add_parser("generate", help="Generate an 8 KB otadata image")
    generate.add_argument("--boot-slot", choices=("app0", "app1"), required=True)
    generate.add_argument("--output", type=Path, required=True)

    args = parser.parse_args()

    if args.command == "inspect":
        print(json.dumps(inspect_otadata(args.path, args.offset), indent=2))
        return 0

    data = make_otadata(args.boot_slot)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(json.dumps(inspect_otadata(args.output), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
