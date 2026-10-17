"""Exercise the preparation CLI with isolated Git SDKs; no ESP-IDF install needed."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IP4 = Path("lwip/src/core/ipv4/ip4.c")
ORIGINAL = b"/* IPv4 fixture */\nint ip4_input(void) {\n    return 0;\n}\n"
PATCHED = ORIGINAL.replace(b"    return 0;", b"    /* transparent hook */\n    return 0;")


def snapshot(directory):
    return {path.relative_to(directory): path.read_bytes()
            for path in directory.rglob("*")
            if path.is_file() and ".git" not in path.relative_to(directory).parts}


class PrepareLwipTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prepare-lwip-test-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.sdk = self.base / "sdk"
        self.source = self.sdk / "components/lwip"
        self.project = self.base / "project"
        self.target = self.project / "components/lwip"
        self.source.mkdir(parents=True)
        (self.sdk / "README").write_bytes(b"Isolated SDK fixture\n")
        idf_commit = self.init_git(self.sdk, "README")
        ip4 = self.source / IP4
        ip4.parent.mkdir(parents=True)
        ip4.write_bytes(ORIGINAL)
        (self.source / "CMakeLists.txt").write_bytes(b"# fixture\n")
        (self.source / "data.bin").write_bytes(b"\x00\xff\r\n\x00\r")
        lwip_commit = self.init_git(self.source / "lwip", "src")
        self.git(self.sdk,"add","components/lwip/CMakeLists.txt","components/lwip/data.bin","components/lwip/lwip")
        self.git(self.sdk,"commit","-m","track component and submodule")
        idf_commit = self.git(self.sdk,"rev-parse","HEAD")
        (self.project / "tools").mkdir(parents=True)
        (self.project / "patches").mkdir()
        self.script = self.project / "tools/prepare_lwip.py"
        shutil.copyfile(ROOT / "tools/prepare_lwip.py", self.script)
        self.patch = self.project / "patches/lwip-v6.1-transparent-tcp.patch"
        self.patch.write_bytes("".join(difflib.unified_diff(
            ORIGINAL.decode().splitlines(keepends=True),
            PATCHED.decode().splitlines(keepends=True),
            fromfile="a/" + IP4.as_posix(), tofile="b/" + IP4.as_posix())).encode())
        (self.project / "patches/lwip-v6.1.json").write_text(json.dumps({
            "idf_commit": idf_commit, "lwip_commit": lwip_commit,
            "file": IP4.as_posix(),
            "original_sha256": hashlib.sha256(ORIGINAL).hexdigest(),
            "patched_sha256": hashlib.sha256(PATCHED).hexdigest(),
        }), encoding="utf-8")

    def git(self, directory, *args):
        return subprocess.check_output([
            "git", "-c", "user.name=Preparation test", "-c", "user.email=test@example.invalid",
            "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
            *args], cwd=directory, text=True, stderr=subprocess.PIPE).strip()

    def init_git(self, directory, tracked):
        self.git(directory, "init")
        self.git(directory, "add", tracked)
        self.git(directory, "commit", "-m", "fixture")
        return self.git(directory, "rev-parse", "HEAD")

    def cli(self, action, error=None, offline=False, optimized=False, git_crlf=False):
        env = os.environ.copy()
        env.pop("IDF_PATH", None)
        if git_crlf:
            # Affect only child Git processes, never the user's global config.
            env.update(GIT_CONFIG_COUNT="2", GIT_CONFIG_KEY_0="core.autocrlf",
                       GIT_CONFIG_VALUE_0="true", GIT_CONFIG_KEY_1="core.eol",
                       GIT_CONFIG_VALUE_1="crlf")
        command = [sys.executable, *(["-O"] if optimized else []), str(self.script), action]
        if not offline:
            command += ["--idf-path", str(self.sdk)]
        result = subprocess.run(command, env=env, cwd=self.project, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1 if error else 0, result.stdout + result.stderr)
        if error:
            self.assertIn(error, result.stderr)
        return result

    def test_lf_lifecycle_and_repeatability(self):
        sdk_before = snapshot(self.sdk)
        self.cli("check", error="Override missing")
        self.cli("apply")
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        applied = snapshot(self.project)
        self.cli("apply")
        self.cli("check")
        self.assertEqual(snapshot(self.project), applied)
        self.cli("restore", offline=True)
        self.cli("restore", offline=True)
        self.assertEqual((self.target / IP4).read_bytes(), ORIGINAL)
        self.cli("check", error="Override restored")
        self.cli("apply")
        self.assertEqual(snapshot(self.project), applied)
        self.assertEqual(snapshot(self.sdk), sdk_before)

    def test_crlf_sdk_and_patch_preserve_source_and_other_files(self):
        (self.source / IP4).write_bytes(ORIGINAL.replace(b"\n", b"\r\n"))
        (self.source / "CMakeLists.txt").write_bytes(b"# fixture\r\n")
        self.patch.write_bytes(self.patch.read_bytes().replace(b"\n", b"\r\n"))
        sdk_before, patch_before = snapshot(self.sdk), self.patch.read_bytes()
        self.cli("apply")
        self.cli("check")
        self.cli("restore", offline=True)
        self.cli("apply")
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        for relative in (Path("CMakeLists.txt"), Path("data.bin")):
            self.assertEqual((self.target / relative).read_bytes(), (self.source / relative).read_bytes())
        self.assertEqual(snapshot(self.sdk), sdk_before)
        self.assertEqual(self.patch.read_bytes(), patch_before)

    def test_mixed_lf_crlf_sdk_is_accepted(self):
        (self.source / IP4).write_bytes(ORIGINAL.replace(b"\n", b"\r\n", 2))
        before = snapshot(self.sdk)
        self.cli("apply")
        self.cli("check")
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        self.assertEqual(snapshot(self.sdk), before)

    def test_git_crlf_config_still_produces_lf_local_copy(self):
        (self.source / IP4).write_bytes(ORIGINAL.replace(b"\n", b"\r\n"))
        before = snapshot(self.sdk)
        self.cli("apply", git_crlf=True)
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        self.cli("check", git_crlf=True)
        self.cli("restore", offline=True, git_crlf=True)
        self.assertEqual((self.target / IP4).read_bytes(), ORIGINAL)
        self.cli("apply", git_crlf=True)
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        self.assertEqual(snapshot(self.sdk), before)

    def test_check_is_read_only_for_existing_crlf_patch(self):
        self.cli("apply")
        (self.target / IP4).write_bytes(PATCHED.replace(b"\n", b"\r\n"))
        before = snapshot(self.project)
        self.cli("check")
        self.assertEqual(snapshot(self.project), before)
        self.cli("apply")
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)

    def test_crlf_local_restore_and_reapply_without_sdk_writes(self):
        self.cli("apply")
        sdk_before = snapshot(self.sdk)
        (self.target / IP4).write_bytes(PATCHED.replace(b"\n", b"\r\n"))
        self.cli("restore", offline=True)
        self.assertEqual((self.target / IP4).read_bytes(), ORIGINAL)
        (self.target / IP4).write_bytes(ORIGINAL.replace(b"\n", b"\r\n"))
        self.cli("restore", offline=True)
        self.assertEqual((self.target / IP4).read_bytes(), ORIGINAL)
        (self.target / IP4).write_bytes(ORIGINAL.replace(b"\n", b"\r\n"))
        self.cli("apply")
        self.assertEqual((self.target / IP4).read_bytes(), PATCHED)
        self.assertEqual(snapshot(self.sdk), sdk_before)

    def test_real_sdk_edits_bom_whitespace_and_lone_cr_are_rejected(self):
        for content in (ORIGINAL.replace(b"return 0", b"return 1"),
                        ORIGINAL.replace(b"    return", b"  return"),
                        b"\xef\xbb\xbf" + ORIGINAL, ORIGINAL.replace(b"\n", b"\r")):
            with self.subTest(content=content):
                (self.source / IP4).write_bytes(content)
                before = snapshot(self.sdk)
                self.cli("apply", error="Global IDF ip4.c has changes beyond LF/CRLF", optimized=True)
                self.assertFalse(self.target.exists())
                self.assertEqual(snapshot(self.sdk), before)

    def test_wrong_idf_commit_is_rejected(self):
        self.git(self.sdk, "commit", "--allow-empty", "-m", "different IDF")
        self.cli("apply", error="ESP-IDF revision mismatch: expected")
        self.assertFalse(self.target.exists())

    def test_wrong_lwip_commit_is_rejected(self):
        self.git(self.source / "lwip", "commit", "--allow-empty", "-m", "different lwIP")
        self.cli("apply", error="esp-lwIP revision mismatch: expected")
        self.assertFalse(self.target.exists())

    def test_other_sdk_file_modifications_are_rejected(self):
        self.cli("apply")
        for path in (self.source/"CMakeLists.txt",self.source/"data.bin"):
            previous=path.read_bytes();path.write_bytes(previous+b"unexpected change")
            before=snapshot(self.project)
            for action in ("apply","check"):
                self.cli(action,error="uncommitted modification")
                self.assertEqual(snapshot(self.project),before)
            path.write_bytes(previous)

    def test_untracked_sdk_file_is_not_silently_copied(self):
        (self.source/"unexpected.c").write_text("int unexpected;\n")
        cache = self.source / "test_apps/.pytest_cache/README.md"
        cache.parent.mkdir(parents=True)
        cache.write_text("SDK test cache\n")
        before = snapshot(self.sdk)
        self.cli("apply")
        self.cli("check")
        self.assertFalse((self.target / "unexpected.c").exists())
        self.assertFalse((self.target / "test_apps").exists())
        self.assertEqual(snapshot(self.sdk), before)

    def test_sdk_or_override_text_checkout_changes_are_accepted_read_only(self):
        self.cli("apply")
        for directory in (self.source, self.target):
            with self.subTest(directory=directory):
                (directory / "CMakeLists.txt").write_bytes(b"# fixture\r\n")
                before = snapshot(self.sdk), snapshot(self.project)
                self.cli("check")
                self.assertEqual((snapshot(self.sdk), snapshot(self.project)), before)
                self.cli("apply")
                (directory / "CMakeLists.txt").write_bytes(b"# fixture\n")

    def test_binary_line_endings_are_not_normalized(self):
        self.cli("apply")
        for directory, error in ((self.source, "uncommitted modification"),
                                 (self.target, "additional modification")):
            with self.subTest(directory=directory):
                path = directory / "data.bin"
                original = path.read_bytes()
                path.write_bytes(original.replace(b"\r\n", b"\n"))
                before = snapshot(self.sdk), snapshot(self.project)
                self.cli("check", error=error)
                self.assertEqual((snapshot(self.sdk), snapshot(self.project)), before)
                path.write_bytes(original)

    def test_other_lwip_tracked_source_modification_is_rejected(self):
        path=self.source/"lwip/src/other.c";path.write_text("int value=0;\n")
        self.git(self.source/"lwip","add","src/other.c")
        self.git(self.source/"lwip","commit","-m","pinned additional file")
        commit=self.git(self.source/"lwip","rev-parse","HEAD")
        self.git(self.sdk,"add","components/lwip/lwip");self.git(self.sdk,"commit","-m","updated pin")
        meta=self.project/"patches/lwip-v6.1.json";values=json.loads(meta.read_text())
        values.update(idf_commit=self.git(self.sdk,"rev-parse","HEAD"),lwip_commit=commit);meta.write_text(json.dumps(values))
        path.write_text("int value=1;\n")
        self.cli("apply",error="uncommitted modification")
        self.assertFalse(self.target.exists())

    def test_local_ip4_edits_are_preserved_for_all_actions(self):
        self.cli("apply")
        (self.target / IP4).write_bytes(PATCHED + b"/* user edit */\n")
        before = snapshot(self.project)
        for action in ("apply", "check", "restore"):
            with self.subTest(action=action):
                self.cli(action, error="Local ip4.c has changes beyond LF/CRLF")
                self.assertEqual(snapshot(self.project), before)

    def test_other_file_edit_is_rejected_before_reapply(self):
        self.cli("apply")
        self.cli("restore")
        (self.target / "CMakeLists.txt").write_bytes(b"# user edit\n")
        before = snapshot(self.project)
        for action in ("apply", "check"):
            self.cli(action, error="additional modification")
            self.assertEqual(snapshot(self.project), before)

    def test_added_or_missing_file_is_rejected(self):
        self.cli("apply")
        extra = self.target / "unexpected.txt"
        extra.write_bytes(b"keep this file\n")
        before = snapshot(self.project)
        self.cli("apply", error="file list differs")
        self.assertEqual(snapshot(self.project), before)
        extra.unlink()
        (self.target / "data.bin").unlink()
        before = snapshot(self.project)
        self.cli("check", error="file list differs")
        self.assertEqual(snapshot(self.project), before)

    def make_symlink(self, path, destination, directory=False):
        try:
            path.symlink_to(destination, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Symlinks unavailable: {error}")

    def test_root_and_dangling_symlinks_are_rejected(self):
        self.target.parent.mkdir(parents=True)
        for destination in (self.source, self.base / "missing"):
            with self.subTest(destination=destination):
                self.make_symlink(self.target, destination, directory=True)
                before = snapshot(self.sdk)
                for action in ("apply", "check", "restore"):
                    self.cli(action, error="local symlink")
                self.assertEqual(snapshot(self.sdk), before)
                self.target.unlink()

    def test_ip4_symlink_cannot_write_to_sdk(self):
        self.cli("apply")
        self.cli("restore")
        local = self.target / IP4
        local.unlink()
        self.make_symlink(local, self.source / IP4)
        before = snapshot(self.sdk)
        for action in ("apply", "check", "restore"):
            self.cli(action, error="local symlink")
            self.assertEqual(snapshot(self.sdk), before)

    def test_other_tracked_sdk_and_override_symlinks_are_rejected(self):
        self.cli("apply")
        for directory in (self.source, self.target):
            with self.subTest(directory=directory):
                path = directory / "CMakeLists.txt"
                original = path.read_bytes()
                destination = self.base / "same-content.txt"
                destination.write_bytes(original)
                path.unlink()
                self.make_symlink(path, destination)
                before = snapshot(self.sdk), snapshot(self.project)
                self.cli("check", error="symlink is not allowed")
                self.assertEqual((snapshot(self.sdk), snapshot(self.project)), before)
                path.unlink()
                path.write_bytes(original)

    def test_bad_patch_leaves_new_and_existing_overrides_untouched(self):
        good_patch = self.patch.read_bytes()
        self.patch.write_bytes(b"not a patch\n")
        self.cli("apply", error="ERROR:")
        self.assertFalse(self.target.exists())
        self.assertEqual(list(self.target.parent.iterdir()), [])
        self.patch.write_bytes(good_patch)
        self.cli("apply")
        self.cli("restore")
        self.patch.write_bytes(b"not a patch\n")
        before = snapshot(self.project)
        self.cli("apply", error="ERROR:")
        self.assertEqual(snapshot(self.project), before)

    def test_wrong_patch_result_hash_is_rejected_before_publish(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b"transparent hook", b"wrong hook"))
        self.cli("apply", error="Local ip4.c has changes beyond LF/CRLF")
        self.assertFalse(self.target.exists())
        self.assertEqual(list(self.target.parent.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
