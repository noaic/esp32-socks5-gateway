"""Release packaging and hardware-free flashing safety checks."""
import contextlib
import copy
import hashlib
import importlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
flash = importlib.import_module("flash_firmware")
packager = importlib.import_module("package_firmware")
REAL_PROVENANCE = packager.provenance


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def firmware_image():
    image = bytearray(256)
    image[:4] = bytes([0xE9, 1, 2, 0x4F])
    struct.pack_into("<H", image, 12, 9)
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[48:48 + len(b"1234567-dirty")] = b"1234567-dirty"
    image[80:80 + len(flash.PROJECT)] = flash.PROJECT.encode("ascii")
    return bytes(image)


def partition_image(partitions=flash.PARTITIONS):
    image = bytearray()
    for category, subtype, offset, size, label, flags in partitions:
        image.extend(struct.pack("<HBBII16sI", 0x50AA, category, subtype, offset, size,
                                 label.encode("ascii"), flags))
    image.extend(b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(image).digest())
    return bytes(image) + b"\xff" * (3072 - len(image))


class FirmwarePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gateway-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.build = Path(self.temp.name) / "build"
        self.output = Path(self.temp.name) / "release"
        (self.root / "tools").mkdir(parents=True)
        (self.root / "LICENSES").mkdir()
        (self.root / "LICENSE").write_text("Project MIT test notice\n")
        (self.root / "THIRD_PARTY_NOTICES.md").write_text("Third party notices\n")
        (self.root / "LICENSES/BSD-3-Clause.txt").write_text("Upstream BSD test notice\n")
        self.config = dict(packager.DEFAULT_PROXY, IDF_TARGET="esp32s3", ESPTOOLPY_FLASHSIZE="16MB",
                           ESPTOOLPY_FLASHMODE="dio", ESPTOOLPY_FLASHFREQ="80m", SPIRAM=True,
                           SPIRAM_MODE_OCT=True, SPIRAM_SPEED=80, PARTITION_TABLE_CUSTOM=True,
                           PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv", ESP_CONSOLE_UART_DEFAULT=True,
                           ESP_CONSOLE_UART_BAUDRATE=115200)
        self.description = {"project_name": flash.PROJECT, "target": "esp32s3",
                            "project_path": str(self.root), "project_version": "1234567-dirty"}
        self.args = {
            "flash_settings": dict(flash.FLASH_SETTINGS),
            "flash_files": {hex(flash.IMAGE_LAYOUT[kind][0]): name for kind, name in packager.SOURCE_FILES.items()},
            "write_flash_args": ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"],
            "extra_esptool_args": {"after": "hard-reset", "before": "default-reset", "stub": True, "chip": "esp32s3"},
        }
        for kind, filename in packager.SOURCE_FILES.items():
            self.args[kind] = {"offset": hex(flash.IMAGE_LAYOUT[kind][0]), "file": filename, "encrypted": "false"}
            path = self.build / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(partition_image() if kind == "partition-table" else firmware_image())
        self.persist()
        (self.build / "sdkconfig").write_text('CONFIG_PROXY_STA_PASSWORD="never-distribute-this"\n')
        self.source = {"git_revision": "a" * 40, "dirty": True, "tree_sha256_at_packaging": "b" * 64,
                       "idf_commit": packager.IDF_COMMIT, "esp_lwip_commit": packager.LWIP_COMMIT}
        patch = mock.patch.object(packager, "provenance", return_value=self.source)
        patch.start()
        self.addCleanup(patch.stop)

    def persist(self):
        write_json(self.build / "config/sdkconfig.json", self.config)
        write_json(self.build / "project_description.json", self.description)
        write_json(self.build / "flasher_args.json", self.args)

    def bundle(self):
        packager.package(self.build, self.output, self.root)
        extracted = Path(self.temp.name) / "extracted"
        with zipfile.ZipFile(self.output / "firmware.zip") as archive:
            archive.extractall(extracted)
        return extracted

    def release_build(self, version="v26.10.16", dirty=False):
        self.description["project_version"] = version
        self.source["dirty"] = dirty
        app = self.build / packager.SOURCE_FILES["app"]
        data = bytearray(app.read_bytes())
        data[48:80] = version.encode().ljust(32, b"\0")
        app.write_bytes(data)
        self.persist()

    def test_bundle_roundtrip_has_script_free_images_and_legal_material(self):
        directory = self.bundle()
        manifest, images = flash.verify_bundle(directory)
        self.assertEqual([offset for offset, _ in images], [0, 0x8000, 0x10000])
        self.assertTrue(manifest["source"]["dirty"])
        self.assertEqual(manifest["build_project_version"], "1234567-dirty")
        self.assertTrue((directory / "LICENSES/BSD-3-Clause.txt").is_file())
        with zipfile.ZipFile(self.output / "firmware.zip") as archive:
            self.assertEqual(len([name for name in archive.namelist() if name.endswith(".bin")]), 5)
            self.assertFalse(any(name.endswith((".py", ".sh", ".ps1", ".exe", ".bat"))
                                 for name in archive.namelist()))
            self.assertNotIn("firmware.zip", archive.namelist())
            self.assertEqual(archive.read("SHA256SUMS"), (self.output / "SHA256SUMS").read_bytes())
            self.assertFalse(any("sdkconfig" in name or "nvs.bin" in name for name in archive.namelist()))
            for name in archive.namelist():
                self.assertNotIn(b"never-distribute-this", archive.read(name))
        for line in (directory / "SHA256SUMS").read_text().splitlines():
            expected, name = line.split("  ")
            self.assertEqual(expected, flash.sha256((directory / name).read_bytes()))
        checked = subprocess.run([sys.executable, str(ROOT / "tools/flash_firmware.py"),
                                  "--directory", str(directory), "--check"],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertIn("No serial port was opened", checked.stdout)
        self.assertFalse((directory / "flash_firmware.py").exists())
        expected, name = (self.output / "firmware.zip.sha256").read_text().strip().split("  ")
        self.assertEqual(name, "firmware.zip")
        self.assertEqual(expected, flash.sha256((self.output / name).read_bytes()))

    def test_factory_merge_and_update_copy_have_explicit_settings_semantics(self):
        directory = self.bundle()
        manifest, images = flash.verify_bundle(directory)
        factory = (directory / "factory.bin").read_bytes()
        app = (directory / "app.bin").read_bytes()
        self.assertEqual(app, (self.build / packager.SOURCE_FILES["app"]).read_bytes())
        self.assertEqual(len(factory), 0x10000 + len(app))
        for offset, path in images:
            self.assertEqual(factory[offset:offset + path.stat().st_size], path.read_bytes())
        self.assertEqual(factory[0x9000:0xF000], b"\xff" * 0x6000)
        self.assertEqual(factory[256:0x8000], b"\xff" * (0x8000 - 256))
        self.assertEqual(manifest["artifacts"]["factory"],
                         {"file": "factory.bin", "offset": 0, "erases_nvs": True})
        self.assertEqual(manifest["artifacts"]["app"],
                         {"file": "app.bin", "offset": 0x10000, "erases_nvs": False,
                          "requires_compatible_layout": True})

    def test_factory_gaps_and_app_copy_checked_even_with_updated_inventory(self):
        directory = self.bundle()
        original_manifest = json.loads((directory / "manifest.json").read_text())
        for name, position in (("factory.bin", 0x9000), ("factory.bin", 0x8000), ("app.bin", 200)):
            with self.subTest(name=name, position=position):
                path = directory / name
                original = path.read_bytes()
                data = bytearray(original)
                data[position] ^= 1
                path.write_bytes(data)
                manifest = copy.deepcopy(original_manifest)
                manifest["files"][name] = {"size": len(data), "sha256": flash.sha256(data)}
                write_json(directory / "manifest.json", manifest)
                with self.assertRaisesRegex(ValueError, "differs"):
                    flash.verify_bundle(directory)
                path.write_bytes(original)
        write_json(directory / "manifest.json", original_manifest)

    def test_helper_requires_explicit_directory(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as rejected:
            flash.main(["--check"])
        self.assertEqual(rejected.exception.code, 2)

    def test_helper_never_writes_factory_image(self):
        directory = self.bundle()
        _, images = flash.verify_bundle(directory)
        write = flash.command("COM5", 115200, images)
        self.assertTrue(all("factory.bin" not in argument and "app.bin" not in argument for argument in write))
        self.assertEqual(write[-6::2], ["0x0", "0x8000", "0x10000"])

    def test_rejects_compiled_private_or_nondefault_project_settings(self):
        for key, value in [("PROXY_STA_SSID", "private-network"), ("PROXY_STA_PASSWORD", "secret"),
                           ("PROXY_AP_SSID", "personal"), ("PROXY_AP_PASSWORD", "private-pass"),
                           ("PROXY_AP_CHANNEL", 11), ("PROXY_LINK_BENCH", True), ("PROXY_UNKNOWN", "secret")]:
            with self.subTest(key=key):
                changed = dict(self.config, **{key: value})
                with self.assertRaises(ValueError) as rejected:
                    packager.validate_config(changed)
                self.assertNotIn(str(value), str(rejected.exception))

    def test_rejects_wrong_target_flash_psram_or_security_mode(self):
        for key, value in [("IDF_TARGET", "esp32"), ("ESPTOOLPY_FLASHSIZE", "8MB"),
                           ("SPIRAM_MODE_OCT", False), ("SPIRAM_SPEED", 40),
                           ("PARTITION_TABLE_CUSTOM_FILENAME", "custom.csv"),
                           ("SECURE_FLASH_ENC_ENABLED", True), ("SECURE_BOOT", True)]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                packager.validate_config(dict(self.config, **{key: value}))

    def test_rejects_extra_nvs_flash_inputs_and_nonstandard_esptool_options(self):
        for change in (lambda value: value["flash_files"].update({"0x9000": "nvs.bin"}),
                       lambda value: value["flash_settings"].update(flash_size="8MB"),
                       lambda value: value["extra_esptool_args"].update(chip="esp32"),
                       lambda value: value["app"].update(encrypted="true"),
                       lambda value: value["flash_files"].update({"0x10000": "../private.bin"})):
            self.args = copy.deepcopy(self.args)
            original = copy.deepcopy(self.args)
            change(self.args)
            self.persist()
            with self.assertRaises(ValueError):
                packager.package(self.build, self.output, self.root)
            self.assertFalse(self.output.exists())
            self.args = original

    def test_rejects_missing_legal_material_and_symlinked_images(self):
        (self.root / "LICENSE").unlink()
        with self.assertRaises(ValueError):
            packager.package(self.build, self.output, self.root)
        (self.root / "LICENSE").write_text("MIT")
        app = self.build / packager.SOURCE_FILES["app"]
        app.unlink()
        try:
            app.symlink_to(ROOT / "tools/flash_firmware.py")
        except OSError:
            self.skipTest("Host does not allow symlinks")
        with self.assertRaises(ValueError):
            packager.package(self.build, self.output, self.root)

    def test_output_cannot_overwrite_existing_artifact(self):
        self.output.write_bytes(b"keep previous bundle")
        with self.assertRaises(ValueError):
            packager.package(self.build, self.output, self.root)
        self.assertEqual(self.output.read_bytes(), b"keep previous bundle")

    def test_optional_release_export_contains_only_two_correctly_named_images(self):
        self.release_build()
        release = Path(self.temp.name) / "public-release"
        with mock.patch.object(packager, "git", return_value=self.source["git_revision"]) as git:
            manifest = packager.package(self.build, self.output, self.root, release=release)
        git.assert_called_once_with(self.root, "rev-parse", "refs/tags/v26.10.16^{commit}")
        prefix = "esp32-socks5-gateway-v26.10.16-esp32s3-n16r8"
        self.assertEqual(sorted(path.name for path in release.iterdir()),
                         [f"{prefix}-factory.bin", f"{prefix}-sysupgrade.bin"])
        self.assertEqual((release / f"{prefix}-factory.bin").read_bytes(),
                         (self.output / "factory.bin").read_bytes())
        self.assertEqual((release / f"{prefix}-sysupgrade.bin").read_bytes(),
                         (self.output / "app.bin").read_bytes())
        self.assertEqual(manifest["build_project_version"], "v26.10.16")
        flash.verify_bundle(self.output)

    def test_release_export_rejects_dirty_invalid_date_or_wrong_tag(self):
        release = Path(self.temp.name) / "public-release"
        for version, dirty in (("v26.10.16", True), ("26.10.16", False),
                               ("v26.02.30", False), ("v26.10.16-1-gabc123", False)):
            with self.subTest(version=version, dirty=dirty):
                self.release_build(version, dirty)
                with self.assertRaises(ValueError):
                    packager.package(self.build, self.output, self.root, release=release)
                self.assertFalse(self.output.exists())
                self.assertFalse(release.exists())
        self.release_build()
        with mock.patch.object(packager, "git", return_value="b" * 40), \
                self.assertRaisesRegex(ValueError, "tag"):
            packager.package(self.build, self.output, self.root, release=release)
        self.assertFalse(self.output.exists())
        self.assertFalse(release.exists())

    def test_release_outputs_cannot_overlap_or_overwrite_existing_files(self):
        self.release_build()
        for release in (self.output, self.output / "nested", self.output.parent):
            with self.subTest(release=release), self.assertRaises(ValueError):
                packager.package(self.build, self.output, self.root, release=release)
            self.assertFalse(self.output.exists())
        release = Path(self.temp.name) / "public-release"
        release.write_bytes(b"existing release")
        with self.assertRaisesRegex(ValueError, "exists"):
            packager.package(self.build, self.output, self.root, release=release)
        self.assertEqual(release.read_bytes(), b"existing release")
        self.assertFalse(self.output.exists())

    def test_failed_release_copy_removes_both_partial_output_directories(self):
        self.release_build()
        release = Path(self.temp.name) / "public-release"
        copytree = packager.shutil.copytree
        def fail_export(*args, **kwargs):
            if Path(args[1]) == release:
                (release / "partial.bin").write_bytes(b"incomplete")
                raise OSError("simulated release copy failure")
            return copytree(*args, **kwargs)
        with mock.patch.object(packager, "git", return_value=self.source["git_revision"]), \
                mock.patch.object(packager.shutil, "copytree", side_effect=fail_export), \
                self.assertRaisesRegex(OSError, "simulated"):
            packager.package(self.build, self.output, self.root, release=release)
        self.assertFalse(self.output.exists())
        self.assertFalse(release.exists())

    def test_app_binary_version_must_match_build_metadata(self):
        app = self.build / packager.SOURCE_FILES["app"]
        data = bytearray(app.read_bytes())
        data[48:80] = b"old-version".ljust(32, b"\0")
        app.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "version"):
            packager.package(self.build, self.output, self.root)

    def test_modified_image_is_rejected_before_serial_access(self):
        directory = self.bundle()
        (directory / "images/esp32-socks5-gateway.bin").write_bytes(b"corrupt")
        with mock.patch.object(flash.subprocess, "run") as serial, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(flash.main(["--directory", str(directory), "--port", "COM5"]), 1)
            serial.assert_not_called()

    def test_manifest_cannot_redirect_write_into_nvs_or_escape_directory(self):
        directory = self.bundle()
        original = json.loads((directory / "manifest.json").read_text())
        for change in (lambda value: value["images"][2].update(offset=0x9000),
                       lambda value: value["images"][2].update(file="../private.bin"),
                       lambda value: value["images"].append(dict(value["images"][0])),
                       lambda value: value["images"][2].update(kind="nvs"),
                       lambda value: value.update(target="esp32"),
                       lambda value: value["artifacts"]["factory"].update(erases_nvs=False),
                       lambda value: value["artifacts"]["app"].update(offset=0x9000),
                       lambda value: value["files"].update({"../private.bin": {"size": 1, "sha256": "0" * 64}})):
            manifest = copy.deepcopy(original)
            change(manifest)
            write_json(directory / "manifest.json", manifest)
            with self.assertRaises(ValueError):
                flash.verify_bundle(directory)

    def test_partition_layout_and_md5_are_checked(self):
        changed = list(flash.PARTITIONS)
        changed[0] = (1, 2, 0x9000, 0x7000, "nvs", 0)
        with self.assertRaises(ValueError):
            flash.validate_image("partition-table", partition_image(changed))
        broken = bytearray(partition_image())
        broken[16] ^= 1
        with self.assertRaisesRegex(ValueError, "MD5"):
            flash.validate_image("partition-table", broken)
        broken = bytearray(firmware_image())
        struct.pack_into("<H", broken, 12, 0)
        with self.assertRaisesRegex(ValueError, "ESP32-S3"):
            flash.validate_image("app", broken)

    def test_check_mode_does_not_import_esptool_or_open_hardware(self):
        directory = self.bundle()
        with mock.patch.object(flash.subprocess, "run") as hardware, \
                mock.patch.object(flash.importlib.metadata, "version") as dependency, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash.main(["--directory", str(directory), "--check"]), 0)
            hardware.assert_not_called()
            dependency.assert_not_called()

    def test_flash_preflight_capacity_and_command_preserve_nvs(self):
        directory = self.bundle()
        _, images = flash.verify_bundle(directory)
        with mock.patch.object(flash.importlib.metadata, "version", return_value=flash.ESPTOOL_VERSION), \
                mock.patch.object(flash.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], 0, "Detected flash size: 16MB\n", ""),
                    subprocess.CompletedProcess([], 0)]) as hardware, contextlib.redirect_stdout(io.StringIO()):
            flash.flash("COM5", 115200, images)
            self.assertEqual(hardware.call_count, 2)
            probe, write = (call.args[0] for call in hardware.call_args_list)
            self.assertIn("flash-id", probe)
            self.assertIn("write-flash", write)
            self.assertNotIn("erase-flash", write)
            self.assertNotIn("--erase-all", write)
            self.assertEqual(write[-6::2], ["0x0", "0x8000", "0x10000"])

    def test_helper_accepts_compatible_esptool_5_versions_without_dependency_changes(self):
        for version in ("5.3.0.dev0", "5.5.0", "5.6.0"):
            with self.subTest(version=version), \
                    mock.patch.object(flash.importlib.metadata, "version", return_value=version), \
                    mock.patch.object(flash.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, "Detected flash size: 16MB\n", ""),
                        subprocess.CompletedProcess([], 0)]) as hardware, \
                    contextlib.redirect_stdout(io.StringIO()):
                flash.flash("COM5", 115200, [])
            self.assertEqual(hardware.call_count, 2)
            self.assertFalse(any("pip" in call.args[0] for call in hardware.call_args_list))

    def test_flash_aborts_on_wrong_capacity_or_dependency(self):
        for capacity in ("8MB", "32MB", "unknown"):
            with self.subTest(capacity=capacity), \
                    mock.patch.object(flash.importlib.metadata, "version", return_value=flash.ESPTOOL_VERSION), \
                    mock.patch.object(flash.subprocess, "run", return_value=
                                      subprocess.CompletedProcess([], 0, f"Detected flash size: {capacity}\n", "")) as hardware, \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
                flash.flash("COM5", 115200, [])
            self.assertEqual(hardware.call_count, 1)
        for version in ("4.0", "6.0"):
            with self.subTest(version=version), \
                    mock.patch.object(flash.importlib.metadata, "version", return_value=version), \
                    mock.patch.object(flash.subprocess, "run") as hardware, \
                    self.assertRaisesRegex(ValueError, "separate virtual environment"):
                flash.flash("COM5", 115200, [])
            hardware.assert_not_called()


class FirmwareProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gateway-provenance-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.idf = Path(self.temp.name) / "sdk"
        ip4 = self.root / "components/lwip/lwip/src/core/ipv4/ip4.c"
        ip4.parent.mkdir(parents=True)
        # Keep host tests independent of an installed SDK/generated override.
        ip4.write_bytes(b"// validated patch fixture\n")
        expected_hash = mock.patch.object(packager, "PATCHED_IP4_SHA256", flash.sha256(ip4.read_bytes()))
        expected_hash.start()
        self.addCleanup(expected_hash.stop)
        (self.root / "tools").mkdir()
        (self.root / "tools/prepare_lwip.py").write_text("# stubbed in test\n")
        (self.root / "main").mkdir()
        (self.root / "main/app.c").write_text("original test source\n")
        self.description = {"idf_path": str(self.idf), "project_version": "1234567-dirty",
                            "build_component_info": {"lwip": {"dir": str(self.root / "components/lwip")}}}

    def git(self, directory, *arguments):
        if directory == self.idf:
            return packager.IDF_COMMIT
        if directory == self.idf / "components/lwip/lwip":
            return packager.LWIP_COMMIT
        return {("rev-parse", "HEAD"): "a" * 40,
                ("describe", "--always", "--tags", "--dirty"): "1234567-dirty",
                ("status", "--porcelain", "--untracked-files=normal"): " M main/app.c"}[arguments]

    def test_provenance_reports_dirty_tree_and_requires_verified_local_override(self):
        with mock.patch.object(packager, "git", side_effect=self.git), \
                mock.patch.object(packager.subprocess, "run") as checker, \
                mock.patch.object(packager.subprocess, "check_output", return_value=b"main/app.c\0"):
            first = REAL_PROVENANCE(self.root, self.description)
            self.assertTrue(first["dirty"])
            self.assertEqual(first["idf_commit"], packager.IDF_COMMIT)
            self.assertIn("check", checker.call_args.args[0])
            (self.root / "main/app.c").write_text("changed test source\n")
            second = REAL_PROVENANCE(self.root, self.description)
            self.assertNotEqual(first["tree_sha256_at_packaging"], second["tree_sha256_at_packaging"])
        wrong = copy.deepcopy(self.description)
        wrong["build_component_info"]["lwip"]["dir"] = str(self.idf / "components/lwip")
        with mock.patch.object(packager, "git", side_effect=self.git), self.assertRaisesRegex(ValueError, "override"):
            REAL_PROVENANCE(self.root, wrong)

    def test_wrong_sdk_lwip_or_stale_build_cannot_be_packaged(self):
        for bad_directory in (self.idf, self.idf / "components/lwip/lwip"):
            def wrong_git(directory, *arguments):
                return "0" * 40 if directory == bad_directory else self.git(directory, *arguments)
            with self.subTest(directory=bad_directory), \
                    mock.patch.object(packager, "git", side_effect=wrong_git), self.assertRaises(ValueError):
                REAL_PROVENANCE(self.root, self.description)
        self.description["project_version"] = "old-version"
        with mock.patch.object(packager, "git", side_effect=self.git), \
                mock.patch.object(packager.subprocess, "run"), self.assertRaisesRegex(ValueError, "stale"):
            REAL_PROVENANCE(self.root, self.description)


if __name__ == "__main__":
    unittest.main()
