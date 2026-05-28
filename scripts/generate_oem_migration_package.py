#!/usr/bin/env python3
"""Generate a local OEM-layout migration package manifest.

This script never talks to hardware. It copies/extracts the binaries needed for
a future guarded migration from the current ProofPro large-slot layout to the
OEM-compatible bootloader/layout and writes a manifest with offsets, sizes, and
SHA-256 hashes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

OEM_FULL_FLASH = ROOT / "firmware-oem" / "smartpid_m5pro_firmware_v2.8.0.bin"
OEM_PARTITIONS = ROOT / "firmware-oem" / "smartpid_m5pro_partitions.bin"
OEM_APP = ROOT / "firmware-oem" / "smartpid_app0.bin"
PROOFPRO_OEM_APP = (
    ROOT / ".pio" / "build" / "m5stack-core-esp32-16M-oem-layout" / "firmware.bin"
)
DEFAULT_OUT_DIR = ROOT / "build" / "migration" / "oem-layout"

OEM_BOOTLOADER_OFFSET = 0x1000
OEM_BOOTLOADER_WINDOW_SIZE = 0x7000
OEM_PARTITION_TABLE_OFFSET = 0x8000
OEM_PARTITION_TABLE_SIZE = 0x0C00
OTA_DATA_OFFSET = 0xE000
OTA_DATA_SIZE = 0x2000
OEM_APP0_OFFSET = 0x10000
OEM_APP1_OFFSET = 0x200000
OEM_APP_SLOT_SIZE = 0x1F0000


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise SystemExit(f"Missing {label}: {path}")


def artifact(path: Path, offset: int, role: str, max_size: int | None = None) -> dict:
    size = path.stat().st_size
    return {
        "role": role,
        "path": str(path.relative_to(ROOT)),
        "offset": offset,
        "offset_hex": f"0x{offset:x}",
        "size": size,
        "size_hex": f"0x{size:x}",
        "max_size": max_size,
        "fits": True if max_size is None else size <= max_size,
        "sha256": sha256(path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate OEM-layout migration package artifacts and manifest."
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help="Output directory for generated package files.",
    )
    parser.add_argument(
        "--boot-app",
        choices=("proofpro", "oem"),
        default="proofpro",
        help="Which app the generated otadata stub should document as boot target.",
    )
    args = parser.parse_args()

    require_file(OEM_FULL_FLASH, "OEM full flash dump")
    require_file(OEM_PARTITIONS, "OEM partition table")
    require_file(OEM_APP, "OEM app image")
    require_file(PROOFPRO_OEM_APP, "ProofPro OEM-layout app image")

    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    full = OEM_FULL_FLASH.read_bytes()
    if len(full) < OEM_BOOTLOADER_OFFSET + OEM_BOOTLOADER_WINDOW_SIZE:
        raise SystemExit("OEM full flash dump is too small to contain bootloader window")

    bootloader_window = full[
        OEM_BOOTLOADER_OFFSET : OEM_BOOTLOADER_OFFSET + OEM_BOOTLOADER_WINDOW_SIZE
    ]
    bootloader = bootloader_window.rstrip(b"\xff")
    if not bootloader or bootloader[0] != 0xE9:
        raise SystemExit("Extracted OEM bootloader does not start with ESP image magic 0xE9")

    bootloader_out = out_dir / "oem_bootloader.bin"
    partitions_out = out_dir / "oem_partitions.bin"
    otadata_out = out_dir / "otadata_boot_proofpro_app0.bin"
    proofpro_out = out_dir / "proofpro_oem_layout_app0.bin"
    oem_app_out = out_dir / "smartpid_oem_app1.bin"

    write_bytes(bootloader_out, bootloader)
    copy_file(OEM_PARTITIONS, partitions_out)
    copy_file(PROOFPRO_OEM_APP, proofpro_out)
    copy_file(OEM_APP, oem_app_out)

    # Keep this intentionally inert for now. The firmware-side conversion writer
    # should generate or verify real OTA data before enabling writes.
    otadata_note = {
        "note": "placeholder only; do not flash as otadata",
        "boot_app": args.boot_app,
        "app0": "proofpro",
        "app1": "smartpid_oem",
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    write_bytes(otadata_out, json.dumps(otadata_note, indent=2).encode("ascii") + b"\n")

    artifacts = [
        artifact(bootloader_out, OEM_BOOTLOADER_OFFSET, "bootloader", OEM_BOOTLOADER_WINDOW_SIZE),
        artifact(partitions_out, OEM_PARTITION_TABLE_OFFSET, "partition_table", OEM_PARTITION_TABLE_SIZE),
        artifact(otadata_out, OTA_DATA_OFFSET, "otadata_placeholder", OTA_DATA_SIZE),
        artifact(proofpro_out, OEM_APP0_OFFSET, "proofpro_app0", OEM_APP_SLOT_SIZE),
        artifact(oem_app_out, OEM_APP1_OFFSET, "smartpid_oem_app1", OEM_APP_SLOT_SIZE),
    ]

    errors: list[str] = []
    for item in artifacts:
        if not item["fits"]:
            errors.append(f"{item['role']} does not fit at {item['offset_hex']}")
    if partitions_out.stat().st_size != OEM_PARTITION_TABLE_SIZE:
        errors.append("OEM partition table backup is not exactly 0x0c00 bytes")
    if otadata_out.stat().st_size > OTA_DATA_SIZE:
        errors.append("otadata placeholder exceeds 0x2000 bytes")

    manifest = {
        "schema": "proofpro_oem_layout_migration_manifest",
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "source": {
            "oem_full_flash": str(OEM_FULL_FLASH.relative_to(ROOT)),
            "oem_partitions": str(OEM_PARTITIONS.relative_to(ROOT)),
            "oem_app": str(OEM_APP.relative_to(ROOT)),
            "proofpro_oem_layout_app": str(PROOFPRO_OEM_APP.relative_to(ROOT)),
        },
        "target_layout": {
            "bootloader": {"offset": OEM_BOOTLOADER_OFFSET, "size": OEM_BOOTLOADER_WINDOW_SIZE},
            "partition_table": {
                "offset": OEM_PARTITION_TABLE_OFFSET,
                "size": OEM_PARTITION_TABLE_SIZE,
            },
            "otadata": {"offset": OTA_DATA_OFFSET, "size": OTA_DATA_SIZE},
            "app0": {
                "offset": OEM_APP0_OFFSET,
                "size": OEM_APP_SLOT_SIZE,
                "payload": "proofpro_oem_layout_app0",
            },
            "app1": {
                "offset": OEM_APP1_OFFSET,
                "size": OEM_APP_SLOT_SIZE,
                "payload": "smartpid_oem_app1",
            },
        },
        "boot_app": args.boot_app,
        "artifacts": artifacts,
        "safe_to_flash": False,
        "safety_note": (
            "Generated package is for offline verification only. Do not flash the "
            "otadata placeholder. Firmware conversion writes remain disabled."
        ),
        "errors": errors,
    }

    manifest_out = out_dir / "manifest.json"
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"Wrote {manifest_out.relative_to(ROOT)}")
    for item in artifacts:
        fit = "fits" if item["fits"] else "DOES NOT FIT"
        print(
            f"{item['role']}: {item['path']} @ {item['offset_hex']} "
            f"{item['size']} bytes ({fit})"
        )
    if errors:
        print("Errors:")
        for err in errors:
            print(f"  - {err}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
