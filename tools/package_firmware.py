#!/usr/bin/env python3
"""Create script-free factory/update firmware assets from a public N16R8 build."""
import argparse
from datetime import date, datetime, timezone
import hashlib
import json
import shutil
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

from flash_firmware import (ESPTOOL_VERSION, FLASH_SETTINGS, IMAGE_LAYOUT, PROJECT,
                            RELEASE_ARTIFACTS, merge_images, sha256, validate_image, verify_bundle)

ROOT = Path(__file__).resolve().parents[1]
PATCH_METADATA = json.loads((ROOT / "patches/lwip-v6.1.json").read_text(encoding="utf-8"))
IDF_COMMIT = PATCH_METADATA["idf_commit"]
LWIP_COMMIT = PATCH_METADATA["lwip_commit"]
PATCHED_IP4_SHA256 = PATCH_METADATA["patched_sha256"]
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

FLASHING = """# ESP32-S3 N16R8 firmware

Only for ESP32-S3 with 16 MB Flash and 8 MB octal PSRAM. No ESP-IDF or
project-specific host script is required. Use official esptool (5.x; 5.5.0 is
the recommended version) or Espressif's Windows Flash Download Tool.

| Image | Offset | Purpose |
| --- | --- | --- |
| factory.bin | 0x0 | First installation/reset: bootloader, partition table and app. Clears saved NVS settings, including Wi-Fi and management credentials. |
| app.bin | 0x10000 | Application-only serial update. Requires a compatible installed bootloader/partition table; normally retains NVS. No OTA support. |
| images/*.bin | 0x0 / 0x8000 / 0x10000 | Advanced three-image update; does not write the NVS range. |

factory.bin fills address gaps (including NVS) with 0xff. It is not a full-chip
dump. Do not flash it to keep existing settings. No erase-flash is needed.
Check that flash-id reports 16 MB; PSRAM is checked by firmware at boot.

For Windows/Linux setup, official flashing commands, USB/serial troubleshooting
and first boot, read:
https://github.com/noaic/esp32-socks5-gateway/blob/main/docs/flash-prebuilt.md
The full developer package uses the short names above; official Release BINs
have a version/hardware prefix and app.bin is named -sysupgrade.bin.

manifest.json records source revision/version and hashes; its esptool_version is
the recommended host flashing version, not the SDK build-tool version. SHA256SUMS covers
payload files and manifest; firmware.zip.sha256 separately covers firmware.zip.
Compare hashes before flashing. Hashes detect corruption, not publisher identity;
download from a trusted source. A dirty source build is for local review.

Keep LICENSE, THIRD_PARTY_NOTICES.md and LICENSES/ when redistributing firmware.
No private NVS, sdkconfig, ELF or executable helper scripts are included.
After a factory reset, connect to ESP32-Transparent-Proxy (esp32proxy), open
http://192.168.4.1/ and set the management password and upstream configuration.
Existing compatible updates keep their previous settings; firmware recovery may
clear incompatible or exhausted NVS.
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


def release_prefix(root, description, source):
    version = description.get("project_version", "")
    if not isinstance(version, str) or not re.fullmatch(r"v[0-9]{2}\.[0-9]{2}\.[0-9]{2}", version):
        raise ValueError("Release export requires a vYY.MM.DD version tag; rebuild that clean tag")
    year, month, day = map(int, version[1:].split("."))
    try:
        date(2000 + year, month, day)
    except ValueError as error:
        raise ValueError("Release version must contain a valid calendar date") from error
    if source.get("dirty") is not False:
        raise ValueError("Release export requires a clean source checkout")
    if git(root, "rev-parse", f"refs/tags/{version}^{{commit}}") != source["git_revision"]:
        raise ValueError("Release tag does not identify the packaged source revision")
    return f"{PROJECT}-{version}-esp32s3-n16r8"


def copy_outputs(outputs):
    """Create exclusive destinations and remove our partial outputs on failure."""
    created = []
    try:
        for staged, output in outputs:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.mkdir()  # Never replace a path another process created.
            created.append(output)
            shutil.copytree(staged, output, dirs_exist_ok=True)
    except BaseException:
        for output in reversed(created):
            shutil.rmtree(output, ignore_errors=True)
        raise


def package(build, output, root=ROOT, release=None):
    for destination in (output, release):
        if destination is not None and (destination.exists() or destination.is_symlink()):
            raise ValueError("Output already exists; choose a new directory or remove the old local artifact")
    root, build, output = root.resolve(), build.resolve(), output.resolve()
    release = release.resolve() if release is not None else None
    if release is not None and (output.is_relative_to(release) or release.is_relative_to(output)):
        raise ValueError("Package and Release output directories must be separate and not nested")
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
    prefix = release_prefix(root, description, source) if release is not None else None
    with tempfile.TemporaryDirectory(prefix="gateway-package-") as temporary:
        staged = Path(temporary) / "bundle"
        staged.mkdir()
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
        outputs = [(staged, output)]
        if release is not None:
            exported = Path(temporary) / "release"
            exported.mkdir()
            for filename, suffix in (("factory.bin", "factory"), ("app.bin", "sysupgrade")):
                shutil.copy2(staged / filename, exported / f"{prefix}-{suffix}.bin")
            outputs.append((exported, release))
        copy_outputs(outputs)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path, help="completed idf.py build directory")
    parser.add_argument("--output-dir", required=True, type=Path, help="new full validation package directory, usually under dist/")
    parser.add_argument("--release-dir", type=Path,
                        help="optional separate new directory for only two versioned BINs; requires a clean vYY.MM.DD tag")
    args = parser.parse_args(argv)
    try:
        manifest = package(args.build_dir, args.output_dir, release=args.release_dir)
        print(f"Created {args.output_dir} ({manifest['source']['git_revision'][:12]}, "
              f"dirty={manifest['source']['dirty']}); script-free factory/app images and firmware.zip")
        if args.release_dir is not None:
            print(f"Exported two versioned factory/sysupgrade BINs to {args.release_dir}; nothing was uploaded")
        return 0
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
