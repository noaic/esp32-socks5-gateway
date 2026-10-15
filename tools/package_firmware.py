#!/usr/bin/env python3
"""Create script-free factory/update firmware assets from a public N16R8 build."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from flash_firmware import (ESPTOOL_VERSION, FLASH_SETTINGS, IMAGE_LAYOUT, PROJECT,
                            RELEASE_ARTIFACTS, merge_images, sha256, validate_image, verify_bundle)

ROOT = Path(__file__).resolve().parents[1]
IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
LWIP_COMMIT = "c6f2f878e7b0f86033214b85547d579be43351e3"
PATCHED_IP4_SHA256 = "410188f50ca79c81a8ba4f396bbef9f15cb0c0cf16a08e2d8623234f88a07267"
SOURCE_FILES = {
    "bootloader": "bootloader/bootloader.bin",
    "partition-table": "partition_table/partition-table.bin",
    "app": "esp32-socks5-gateway.bin",
}
DEFAULT_PROXY = {
    "PROXY_STA_SSID": "", "PROXY_STA_PASSWORD": "",
    "PROXY_AP_SSID": "ESP32-Transparent-Proxy", "PROXY_AP_PASSWORD": "esp32proxy",
    "PROXY_AP_CHANNEL": 6, "PROXY_POC_ENABLE": True, "PROXY_TCP_SOCKS5": True,
    "PROXY_UDP_ENABLE": True, "PROXY_POC_TEST_IP": "198.51.100.7",
    "PROXY_VERBOSE_DIAGNOSTICS": False, "PROXY_LINK_PROBE": False,
    "PROXY_PIPELINE_DIAG": False, "PROXY_LINK_BENCH": False,
}

FLASHING = r"""# ESP32-S3 N16R8 firmware

Only for ESP32-S3 with 16 MB Flash and 8 MB octal PSRAM. Read manifest.json
for the build version and source revision; a dirty build is a local review
artifact. Download from a trusted source. SHA256 detects damaged files but
cannot authenticate a publisher. Keep LICENSE, THIRD_PARTY_NOTICES.md and
LICENSES/ with redistributed firmware. No ESP-IDF or project script is needed.

## Choose the correct image

| File | Flash offset | Use and saved configuration |
| --- | --- | --- |
| factory.bin | 0x0 | Initial installation or reset. Includes bootloader, partition table and app. Erases NVS, including saved Wi-Fi, SOCKS5 and management settings. |
| app.bin | 0x10000 | Update only when the installed bootloader and partition layout are compatible with this release. Normally preserves NVS. |
| images/*.bin | 0x0 / 0x8000 / 0x10000 | Advanced split-image flashing. Writes the bootloader, partition table and app without writing the NVS range. |

factory.bin is a merged raw image, not a 16 MB device dump. Its address gaps,
including NVS at 0x9000-0xefff, contain 0xff and are erased when flashed. Do not
choose factory.bin when you intend to keep settings. app.bin alone cannot boot
a blank device. Compare the installed release's partition and compatibility
notes before an app-only update. If compatibility is unknown, save the settings
and use factory.bin, then configure the device again. No saved NVS, private
credentials, sdkconfig, ELF files or executable scripts are distributed.

## Verify the download

Extract firmware.zip into a new directory, or download all loose files together.
SHA256SUMS covers the payload files and manifest (excluding itself and the ZIP).
firmware.zip.sha256 beside the archive covers the ZIP separately.

Windows PowerShell:

```powershell
Get-FileHash .\firmware.zip -Algorithm SHA256
Get-Content .\firmware.zip.sha256
Get-FileHash .\factory.bin -Algorithm SHA256
Select-String '  factory.bin$' .\SHA256SUMS
```

Linux Bash:

```bash
sha256sum -c firmware.zip.sha256
sha256sum -c SHA256SUMS
```

The ZIP checksum sidecar is a separate download, not inside the ZIP. If you only
downloaded loose files, skip the ZIP checksum command. If you intend to flash
app.bin, compare its hash against its entry in SHA256SUMS instead.

## Flash with official esptool

Use Python 3.10 or newer; 64-bit Python 3.12 is recommended. Install the official
esptool package in an isolated environment. Windows 11 PowerShell, from the
extracted/download directory (replace COM5 with the actual serial port):

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool==5.5.0
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 flash-id
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 factory.bin
```

For a compatible app-only update, run this INSTEAD of the factory command:

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x10000 app.bin
```

Linux Bash (replace /dev/ttyACM0 with the actual serial port):

```bash
python3 -m venv .venv
.venv/bin/python -m pip install esptool==5.5.0
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash-id
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x0 factory.bin
```

For a compatible app-only update, run this INSTEAD of the factory command:

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x10000 app.bin
```

Check that flash-id reports 16MB before writing. PSRAM is checked at firmware
boot, not by flash-id. On Ubuntu/Debian you may need python3-venv. Serial-device
access follows the distribution's group rules. WSL2 needs USB passthrough;
flashing directly from Windows is usually easier. Close serial monitors first.
Install the board's USB-UART driver if no port appears. If automatic download
mode fails, hold BOOT, tap RESET, release BOOT and retry. Increase --baud to
460800 only with a reliable connection. No erase-flash command is required.

On Windows, Espressif's official Flash Download Tool is a GUI alternative:
https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/production_stage/tools/flash_download_tool.html
Select ESP32-S3 and factory.bin at 0x0. Keep the original image parameters
(enable DoNotChgBin if shown); the image is already DIO, 80 MHz and 16 MB.

Advanced split flashing uses the same official write-flash command with:

```text
0x0 images/bootloader.bin 0x8000 images/partition-table.bin 0x10000 images/esp32-socks5-gateway.bin
```

The optional repository helper tools/flash_firmware.py --directory <directory>
--check verifies this layout. With --port it writes only these split images;
it is not included in this release and is not required for normal flashing.

## After flashing

After a factory install/reset, connect to Wi-Fi ESP32-Transparent-Proxy (password
esp32proxy), open http://192.168.4.1/ and create the management password. Configure
upstream Wi-Fi and SOCKS5 in the Web panel. Do not leave public defaults active
around untrusted users. Compatible app-only or split updates normally retain
the device's previous AP address, password and settings. Startup recovery can
clear exhausted/incompatible NVS or restore defaults for invalid settings.

The firmware console uses UART0 at 115200, which may be a different connector
from native USB flashing. Use the board's UART bridge for console output:

```text
python -m serial.tools.miniterm <serial-port> 115200 --raw
```

Use the same virtual environment's Python executable as above. Exit miniterm
with Ctrl+]. Keep terminal local echo off; the firmware echoes input. Other
serial terminals should use 115200 8N1 and Enter/CRLF.
"""


def git(directory, *arguments):
    return subprocess.check_output(["git", "-C", str(directory), *arguments],
                                   stderr=subprocess.PIPE).decode("utf-8").strip()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def regular_file(root, relative):
    path = root / relative
    if path.is_symlink() or any(parent.is_symlink() for parent in path.parents if parent != root):
        raise ValueError(f"Symlink is not allowed: {path}")
    if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Missing file or path outside expected directory: {path}")
    return path


def validate_config(config):
    required = {
        "IDF_TARGET": "esp32s3", "ESPTOOLPY_FLASHSIZE": "16MB",
        "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHFREQ": "80m",
        "SPIRAM": True, "SPIRAM_MODE_OCT": True, "SPIRAM_SPEED": 80,
        "PARTITION_TABLE_CUSTOM": True, "PARTITION_TABLE_CUSTOM_FILENAME": "partitions.csv",
        "ESP_CONSOLE_UART_DEFAULT": True, "ESP_CONSOLE_UART_BAUDRATE": 115200,
    }
    for key, expected in required.items():
        if config.get(key) != expected:
            raise ValueError(f"Unsupported build configuration: CONFIG_{key}")
    for key, expected in DEFAULT_PROXY.items():
        if config.get(key, False) != expected:
            # Do not include the actual value: it may be a private credential.
            raise ValueError(f"Public bundles require default CONFIG_{key}; rebuild from sdkconfig.defaults")
    if any(key.startswith("PROXY_") and key not in DEFAULT_PROXY for key in config):
        raise ValueError("Unknown project setting; review privacy/defaults before packaging")
    if config.get("SECURE_BOOT") or config.get("SECURE_FLASH_ENC_ENABLED"):
        raise ValueError("This packager does not support secure-boot or encrypted flash images")


def provenance(root, description):
    idf = Path(description["idf_path"])
    if git(idf, "rev-parse", "HEAD") != IDF_COMMIT:
        raise ValueError("Build SDK is not the required exact ESP-IDF commit")
    if git(idf / "components/lwip/lwip", "rev-parse", "HEAD") != LWIP_COMMIT:
        raise ValueError("Build SDK is not the required exact esp-lwIP commit")
    override = root / "components/lwip"
    if Path(description["build_component_info"]["lwip"]["dir"]).resolve() != override.resolve():
        raise ValueError("Build did not use the project-local lwIP override")
    ip4 = regular_file(override, "lwip/src/core/ipv4/ip4.c")
    if sha256(ip4.read_bytes().replace(b"\r\n", b"\n")) != PATCHED_IP4_SHA256:
        raise ValueError("Project-local lwIP patch does not match the supported version")
    subprocess.run([sys.executable, str(regular_file(root, "tools/prepare_lwip.py")), "check", "--idf-path", str(idf)],
                   check=True, capture_output=True, text=True)
    revision = git(root, "rev-parse", "HEAD")
    if description.get("project_version") != git(root, "describe", "--always", "--tags", "--dirty"):
        raise ValueError("Build version is stale or custom; reconfigure and rebuild the current source before packaging")
    dirty = bool(git(root, "status", "--porcelain", "--untracked-files=normal"))
    names = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z", "--cached",
                                     "--others", "--exclude-standard"]).split(b"\0")
    tree = hashlib.sha256()
    for name in sorted(set(name for name in names if name)):
        path = root / name.decode("utf-8")
        if not path.exists():
            continue  # A deleted tracked file is represented by its absence.
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Unsupported source file: {path}")
        tree.update(name + b"\0" + hashlib.sha256(path.read_bytes()).digest())
    return {"git_revision": revision, "dirty": dirty, "tree_sha256_at_packaging": tree.hexdigest(),
            "idf_commit": IDF_COMMIT, "esp_lwip_commit": LWIP_COMMIT,
            "patched_ip4_sha256": PATCHED_IP4_SHA256}


def package(build, output, root=ROOT):
    root, build, output = root.resolve(), build.resolve(), output.resolve()
    if output.exists():
        raise ValueError("Output already exists; choose a new directory or remove the old local artifact")
    description = read_json(regular_file(build, "project_description.json"))
    if (description.get("project_name") != PROJECT or description.get("target") != "esp32s3"
            or Path(description.get("project_path", "")).resolve() != root):
        raise ValueError("Build metadata does not identify this project and target")
    validate_config(read_json(regular_file(build, "config/sdkconfig.json")))
    args = read_json(regular_file(build, "flasher_args.json"))
    expected_files = {hex(IMAGE_LAYOUT[kind][0]): name for kind, name in SOURCE_FILES.items()}
    if (args.get("flash_files") != expected_files or args.get("flash_settings") != FLASH_SETTINGS
            or args.get("write_flash_args") != ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"]
            or args.get("extra_esptool_args") != {
                "after": "hard-reset", "before": "default-reset", "stub": True, "chip": "esp32s3"}):
        raise ValueError("Unexpected flash inputs/options; only bootloader, partitions and app are permitted")
    for kind, filename in SOURCE_FILES.items():
        if args.get(kind) != {"offset": hex(IMAGE_LAYOUT[kind][0]), "file": filename, "encrypted": "false"}:
            raise ValueError(f"Unexpected {kind} flash metadata")
    source = provenance(root, description)
    with tempfile.TemporaryDirectory(prefix="gateway-package-") as temporary:
        staged = Path(temporary)
        images = []
        for kind, filename in SOURCE_FILES.items():
            data = regular_file(build, filename).read_bytes()
            validate_image(kind, data)
            if kind == "app" and (not isinstance(description.get("project_version"), str)
                                  or data[48:80].split(b"\0", 1)[0].decode("utf-8")
                                  != description["project_version"][:31]):
                raise ValueError("Application image version does not match the build metadata")
            offset, destination, capacity = IMAGE_LAYOUT[kind]
            if not 0 < len(data) <= capacity:
                raise ValueError(f"{kind} does not fit its safe flash region")
            target = staged / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            images.append({"kind": kind, "offset": offset, "file": destination})
        split = [(image["offset"], (staged / image["file"]).read_bytes()) for image in images]
        (staged / "factory.bin").write_bytes(merge_images(split))
        (staged / "app.bin").write_bytes((staged / IMAGE_LAYOUT["app"][1]).read_bytes())
        (staged / "FLASHING.md").write_text(FLASHING, encoding="utf-8", newline="\n")
        licenses = ["LICENSE", "THIRD_PARTY_NOTICES.md"]
        if not (root / "LICENSES").is_dir():
            raise ValueError("Missing third-party LICENSES directory")
        licenses.extend(path.relative_to(root).as_posix() for path in sorted((root / "LICENSES").rglob("*"))
                        if path.is_file() or path.is_symlink())
        if len(licenses) <= 2:
            raise ValueError("Third-party LICENSES directory is empty")
        for filename in licenses:
            data = regular_file(root, filename).read_bytes()
            data.decode("utf-8")  # Legal bundle must be readable text, not arbitrary generated artifacts.
            target = staged / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        manifest = {"schema": 2, "project": PROJECT, "target": "esp32s3", "board": "ESP32-S3 N16R8",
                    "psram_mb": 8, "flash_settings": args["flash_settings"], "esptool_version": ESPTOOL_VERSION,
                    "created_utc": datetime.now(timezone.utc).isoformat(), "source": source,
                    "build_project_version": description.get("project_version"), "images": images,
                    "artifacts": RELEASE_ARTIFACTS,
                    "credentials": "public build defaults; no sdkconfig or NVS image included", "files": {}}
        for path in sorted(staged.rglob("*")):
            if path.is_file():
                data = path.read_bytes()
                manifest["files"][path.relative_to(staged).as_posix()] = {"size": len(data), "sha256": sha256(data)}
        (staged / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        verify_bundle(staged)
        checksums = []
        for path in sorted(staged.rglob("*")):
            if path.is_file():
                checksums.append(f"{sha256(path.read_bytes())}  {path.relative_to(staged).as_posix()}")
        (staged / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="utf-8")
        # The payload checksums are identical in the loose files and inside ZIP.
        # The archive has a separate sidecar, avoiding recursive checksum files.
        archive_path = staged / "firmware.zip"
        payload = [path for path in sorted(staged.rglob("*")) if path.is_file()]
        with zipfile.ZipFile(archive_path, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in payload:
                archive.write(path, path.relative_to(staged).as_posix())
        (staged / "firmware.zip.sha256").write_text(
            f"{sha256(archive_path.read_bytes())}  firmware.zip\n", encoding="utf-8")
        output.parent.mkdir(parents=True, exist_ok=True)
        # copytree creates the directory exclusively; never overwrite prior artifacts.
        shutil.copytree(staged, output)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path, help="completed idf.py build directory")
    parser.add_argument("--output-dir", required=True, type=Path, help="new release directory, usually under dist/")
    args = parser.parse_args(argv)
    try:
        manifest = package(args.build_dir, args.output_dir)
        print(f"Created {args.output_dir} ({manifest['source']['git_revision'][:12]}, "
              f"dirty={manifest['source']['dirty']}); script-free factory/app images and firmware.zip")
        return 0
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
