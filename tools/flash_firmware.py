#!/usr/bin/env python3
"""Optional developer helper: verify a release and flash its three split images.

Ordinary users can flash factory.bin or app.bin with official esptool directly.
This repository-only helper never writes factory.bin or the NVS address range.
The manifest checks integrity, not publisher identity; use a trusted release.
"""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys

ESPTOOL_VERSION = "5.5.0"
PROJECT = "esp32-socks5-gateway"
FLASH_SETTINGS = {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"}
IMAGE_LAYOUT = {
    "bootloader": (0x0, "images/bootloader.bin", 0x8000),
    "partition-table": (0x8000, "images/partition-table.bin", 0x1000),
    "app": (0x10000, "images/esp32-socks5-gateway.bin", 0x400000),
}
RELEASE_ARTIFACTS = {
    "factory": {"file": "factory.bin", "offset": 0, "erases_nvs": True},
    "app": {"file": "app.bin", "offset": 0x10000, "erases_nvs": False,
            "requires_compatible_layout": True},
}
PARTITIONS = [
    (1, 2, 0x9000, 0x6000, "nvs", 0),
    (1, 1, 0xF000, 0x1000, "phy_init", 0),
    (0, 0, 0x10000, 0x400000, "factory", 0),
]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_file(root, name):
    if not isinstance(name, str):
        raise ValueError("Package path must be a string")
    relative = PurePosixPath(name)
    if (not name or "\\" in name or ":" in name or relative.is_absolute()
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError(f"Unsafe package path: {name!r}")
    path = root
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Package symlink is not allowed: {name}")
    if not path.is_file():
        raise ValueError(f"Package file missing: {name}")
    return path


def validate_image(kind, data):
    if kind == "partition-table":
        found = []
        md5_seen = False
        for position in range(0, len(data), 32):
            record = data[position:position + 32]
            if len(record) != 32:
                raise ValueError("Truncated partition table")
            magic = struct.unpack_from("<H", record)[0]
            if magic == 0xFFFF:
                if any(byte != 0xFF for byte in data[position:]):
                    raise ValueError("Unexpected bytes after partition table")
                break
            if magic == 0xEBEB and not md5_seen:
                expected = hashlib.md5(data[:position]).digest()
                if record[2:16] != b"\xff" * 14 or record[16:] != expected:
                    raise ValueError("Partition table MD5 mismatch")
                md5_seen = True
                continue
            if magic != 0x50AA or md5_seen:
                raise ValueError("Unexpected partition record")
            _, category, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", record)
            found.append((category, subtype, offset, size, label.rstrip(b"\0").decode("ascii"), flags))
        if found != PARTITIONS or not md5_seen:
            raise ValueError("Partition table differs from the supported NVS/PHY/factory layout")
    else:
        if (len(data) < 24 or data[0] != 0xE9 or not 1 <= data[1] <= 16
                or struct.unpack_from("<H", data, 12)[0] != 9):
            raise ValueError(f"{kind}: expected an ESP32-S3 image")
        # ESP image header: DIO=2, 16 MB=4, 80 MHz=0xf.
        if data[2] != 2 or data[3] != 0x4F:
            raise ValueError(f"{kind}: expected DIO / 16 MB / 80 MHz image header")
        if kind == "app" and (len(data) < 176 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432
                              or data[80:112].split(b"\0", 1)[0] != PROJECT.encode("ascii")):
            raise ValueError("Application descriptor does not identify this project")


def merge_images(images):
    """Merge validated (offset, bytes) pairs, filling address gaps with 0xff.

    The raw result covers NVS even though no saved NVS image is an input.
    Flashing it at zero therefore resets settings; split writes avoid that range.
    """
    merged = bytearray()
    for offset, data in sorted(images):
        if offset < len(merged):
            raise ValueError("Overlapping images cannot form a factory image")
        merged.extend(b"\xff" * (offset - len(merged)))
        merged.extend(data)
    return bytes(merged)


def verify_bundle(root):
    root = root.resolve()
    manifest_path = safe_file(root, "manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (not isinstance(manifest, dict) or manifest.get("schema") != 2 or manifest.get("project") != PROJECT
            or manifest.get("target") != "esp32s3" or manifest.get("board") != "ESP32-S3 N16R8"
            or manifest.get("psram_mb") != 8 or manifest.get("flash_settings") != FLASH_SETTINGS
            or manifest.get("esptool_version") != ESPTOOL_VERSION):
        raise ValueError("Unsupported package, board, flash settings or esptool version")
    inventory = manifest.get("files")
    if not isinstance(inventory, dict) or not inventory:
        raise ValueError("Missing package file inventory")
    for name, entry in inventory.items():
        data = safe_file(root, name).read_bytes()
        if (not isinstance(entry, dict) or type(entry.get("size")) is not int
                or len(data) != entry["size"] or sha256(data) != entry.get("sha256")):
            raise ValueError(f"Size or SHA256 mismatch: {name}")
    required = {layout[1] for layout in IMAGE_LAYOUT.values()} | {
        "factory.bin", "app.bin", "FLASHING.md", "LICENSE", "THIRD_PARTY_NOTICES.md"}
    if not required.issubset(inventory):
        raise ValueError("Required firmware or license file missing from inventory")
    if manifest.get("artifacts") != RELEASE_ARTIFACTS:
        raise ValueError("Unsupported factory/update offsets or settings-preservation metadata")
    if any(name.endswith((".py", ".sh", ".exe", ".bat", ".ps1")) for name in inventory):
        raise ValueError("Firmware releases must not contain executable helper scripts")
    images = manifest.get("images")
    if not isinstance(images, list) or len(images) != len(IMAGE_LAYOUT):
        raise ValueError("Expected exactly bootloader, partition table and application images")
    seen = set()
    validated = []
    for image in images:
        if not isinstance(image, dict) or image.get("kind") not in IMAGE_LAYOUT:
            raise ValueError("Unexpected flash image")
        kind = image["kind"]
        offset, name, capacity = IMAGE_LAYOUT[kind]
        if (kind in seen or image.get("file") != name
                or type(image.get("offset")) is not int or image["offset"] != offset):
            raise ValueError("Duplicate image, unsafe path or unsupported flash offset")
        seen.add(kind)
        path = safe_file(root, name)
        data = path.read_bytes()
        if not 0 < len(data) <= capacity:
            raise ValueError(f"{kind}: image exceeds its safe flash region")
        validate_image(kind, data)
        if kind == "app" and (not isinstance(manifest.get("build_project_version"), str)
                              or data[48:80].split(b"\0", 1)[0].decode("utf-8")
                              != manifest["build_project_version"][:31]):
            raise ValueError("Application version does not match the package manifest")
        validated.append((offset, path))
    split = [(offset, path.read_bytes()) for offset, path in validated]
    if safe_file(root, "factory.bin").read_bytes() != merge_images(split):
        raise ValueError("Factory image differs from the split images or 0xff address gaps")
    if safe_file(root, "app.bin").read_bytes() != safe_file(root, IMAGE_LAYOUT["app"][1]).read_bytes():
        raise ValueError("Application update image differs from the validated application")
    return manifest, sorted(validated)


def command(port, baud, images):
    result = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
              "--baud", str(baud), "--before", "default-reset", "--after", "hard-reset",
              "write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"]
    for offset, path in images:
        result.extend([hex(offset), str(path)])
    return result


def flash(port, baud, images):
    try:
        installed = importlib.metadata.version("esptool")
    except importlib.metadata.PackageNotFoundError:
        installed = "not installed"
    if installed != ESPTOOL_VERSION:
        raise ValueError(f"esptool {installed}; install the tested version with: "
                         f"python -m pip install esptool=={ESPTOOL_VERSION}")
    if not port or port.startswith("-"):
        raise ValueError("A serial port such as COM5 or /dev/ttyACM0 is required")
    # Check the real flash capacity before programming. PSRAM cannot be checked
    # by the ROM loader; firmware checks the required 8 MB PSRAM at startup.
    probe = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
             "--baud", str(baud), "--before", "default-reset", "--after", "hard-reset", "flash-id"]
    checked = subprocess.run(probe, capture_output=True, text=True, check=True)
    print(checked.stdout, end="", flush=True)
    if not re.search(r"Detected flash size:\s*16MB\b", checked.stdout):
        raise ValueError("Device did not report exactly 16 MB flash; refusing to write")
    subprocess.run(command(port, baud, images), check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path,
                        help="extracted firmware directory; the helper stays in repository tools/")
    parser.add_argument("--check", action="store_true", help="verify files and layout; do not open a serial port")
    parser.add_argument("--port", help="serial port, for example COM5 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, choices=(115200, 230400, 460800, 921600), default=115200,
                        help="flash baud rate (default: 115200)")
    args = parser.parse_args(argv)
    if not args.check and not args.port:
        parser.error("use --check to verify only, or --port to flash")
    try:
        manifest, images = verify_bundle(args.directory)
        print(f"Verified {manifest['project']} for {manifest['board']}; "
              "factory/app artifacts and 3 split images; helper writes split images only.", flush=True)
        if args.check:
            print("No serial port was opened. SHA256 detects corruption, not an untrusted publisher.")
            return 0
        flash(args.port, args.baud, images)
        print("Flashing complete. The helper did not write NVS; settings normally remain, "
              "but firmware may clear incompatible or exhausted NVS during recovery. "
              "First boot: connect to ESP32-Transparent-Proxy and open http://192.168.4.1/; "
              "an already configured device keeps its previous network settings.")
        return 0
    except (ValueError, OSError, UnicodeError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
