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
import struct
from datetime import datetime, timezone
from pathlib import Path

from otadata_tool import make_otadata


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
OEM_EEPROM_OFFSET = 0x3FF000
OEM_EEPROM_SIZE = 0x1000
PACKAGE_MAGIC = b"PPMIG001"
PACKAGE_HEADER_SIZE = 12


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


def make_package(path: Path, manifest: dict, artifact_items: list[dict]) -> None:
    manifest_payload = json.dumps(manifest, indent=2).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(PACKAGE_MAGIC)
        f.write(struct.pack("<I", len(manifest_payload)))
        f.write(manifest_payload)
        for item in artifact_items:
            f.write((ROOT / item["path"]).read_bytes() if not Path(item["path"]).is_absolute()
                    else Path(item["path"]).read_bytes())


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise SystemExit(f"Missing {label}: {path}")


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def artifact(path: Path, offset: int, role: str, max_size: int | None = None) -> dict:
    size = path.stat().st_size
    return {
        "role": role,
        "path": display_path(path),
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
        help="Which app the generated OEM-layout otadata should boot.",
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

    boot_slot = "app0" if args.boot_app == "proofpro" else "app1"
    bootloader_out = out_dir / "oem_bootloader.bin"
    partitions_out = out_dir / "oem_partitions.bin"
    otadata_out = out_dir / f"otadata_boot_{args.boot_app}_{boot_slot}.bin"
    proofpro_out = out_dir / "proofpro_oem_layout_app0.bin"
    oem_app_out = out_dir / "smartpid_oem_app1.bin"
    eeprom_out = out_dir / "smartpid_oem_eeprom.bin"
    package_out = out_dir / "proofpro_oem_layout_migration.ppmig"

    write_bytes(bootloader_out, bootloader)
    copy_file(OEM_PARTITIONS, partitions_out)
    copy_file(PROOFPRO_OEM_APP, proofpro_out)
    copy_file(OEM_APP, oem_app_out)
    write_bytes(eeprom_out, full[OEM_EEPROM_OFFSET : OEM_EEPROM_OFFSET + OEM_EEPROM_SIZE])

    write_bytes(otadata_out, make_otadata(boot_slot))

    artifacts = [
        artifact(proofpro_out, OEM_APP0_OFFSET, "proofpro_app0", OEM_APP_SLOT_SIZE),
        artifact(oem_app_out, OEM_APP1_OFFSET, "smartpid_oem_app1", OEM_APP_SLOT_SIZE),
        artifact(eeprom_out, OEM_EEPROM_OFFSET, "smartpid_oem_eeprom", OEM_EEPROM_SIZE),
        artifact(partitions_out, OEM_PARTITION_TABLE_OFFSET, "partition_table", OEM_PARTITION_TABLE_SIZE),
        artifact(bootloader_out, OEM_BOOTLOADER_OFFSET, "bootloader", OEM_BOOTLOADER_WINDOW_SIZE),
        artifact(otadata_out, OTA_DATA_OFFSET, f"otadata_boot_{boot_slot}", OTA_DATA_SIZE),
    ]

    errors: list[str] = []
    for item in artifacts:
        if not item["fits"]:
            errors.append(f"{item['role']} does not fit at {item['offset_hex']}")
    if partitions_out.stat().st_size != OEM_PARTITION_TABLE_SIZE:
        errors.append("OEM partition table backup is not exactly 0x0c00 bytes")
    if otadata_out.stat().st_size != OTA_DATA_SIZE:
        errors.append("generated otadata is not exactly 0x2000 bytes")

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
            "eeprom": {
                "offset": OEM_EEPROM_OFFSET,
                "size": OEM_EEPROM_SIZE,
                "payload": "smartpid_oem_eeprom",
            },
        },
        "boot_app": args.boot_app,
        "boot_slot": boot_slot,
        "package_format": {
            "magic": PACKAGE_MAGIC.decode("ascii"),
            "header_size": PACKAGE_HEADER_SIZE,
            "manifest_length_encoding": "uint32_le",
            "payload_order": "artifacts array order",
        },
        "artifacts": artifacts,
        "safe_to_flash": False,
        "safety_note": (
            "Generated package is for offline verification only. Firmware conversion "
            "writes remain disabled until the full on-device sequencing is implemented."
        ),
        "errors": errors,
    }

    manifest_out = out_dir / "manifest.json"
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    make_package(package_out, manifest, artifacts)

    print(f"Wrote {display_path(manifest_out)}")
    print(f"Wrote {display_path(package_out)} ({package_out.stat().st_size} bytes)")
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
