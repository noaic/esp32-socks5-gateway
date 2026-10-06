"""Create/check/restore a project-local lwIP override; never write to IDF_PATH."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "patches/lwip-v6.1-transparent-tcp.patch"
META = json.loads((ROOT / "patches/lwip-v6.1.json").read_text())
TARGET = ROOT / "components/lwip"


def run(*args, cwd):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def state(directory):
    value = digest(directory / META["file"])
    if value == META["original_sha256"]:
        return "original"
    if value == META["patched_sha256"]:
        return "patched"
    raise RuntimeError("Local ip4.c differs from both pinned states; refusing to overwrite it")


def apply_patch(directory, reverse=False):
    args = ["git", "apply"] + (["--reverse"] if reverse else [])
    run(*args, "--check", str(PATCH), cwd=directory)
    run(*args, str(PATCH), cwd=directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("apply", "check", "restore"))
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if args.action == "restore":
        if not TARGET.exists():
            print("No project-local override to restore")
            return
        if TARGET.is_symlink():
            raise RuntimeError("Refusing to touch a symlink")
        if state(TARGET) == "patched":
            apply_patch(TARGET, reverse=True)
        assert state(TARGET) == "original"
        print("Restored project-local ip4.c; baseline requires PoC disabled; reapply before menuconfig if still enabled")
        return
    if not args.idf_path:
        raise RuntimeError("Activate ESP-IDF first, or provide --idf-path")
    idf = Path(args.idf_path).resolve()
    source = idf / "components/lwip"
    if run("git", "rev-parse", "HEAD", cwd=idf) != META["idf_commit"]:
        raise RuntimeError("ESP-IDF revision mismatch; review/rebase patch before using another version")
    if run("git", "rev-parse", "HEAD", cwd=source / "lwip") != META["lwip_commit"]:
        raise RuntimeError("esp-lwIP revision mismatch")
    if digest(source / META["file"]) != META["original_sha256"]:
        raise RuntimeError("Global IDF ip4.c is modified; refusing to copy")
    if not TARGET.exists():
        if args.action == "check":
            raise RuntimeError("Override missing; run prepare_lwip.py apply")
        TARGET.parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="lwip-stage-", dir=TARGET.parent) as temporary:
            staged = Path(temporary) / "lwip"
            shutil.copytree(source, staged, ignore=shutil.ignore_patterns(".git", "__pycache__"))
            apply_patch(staged)
            assert state(staged) == "patched"
            staged.rename(TARGET)
    else:
        if TARGET.is_symlink():
            raise RuntimeError("Refusing to touch a symlink")
        if state(TARGET) == "original":
            if args.action == "check":
                raise RuntimeError("Override restored; run prepare_lwip.py apply")
            apply_patch(TARGET)
    assert state(TARGET) == "patched"
    # Check the complete copied component, not only the patched file.
    def files(directory):
        return {path.relative_to(directory) for path in directory.rglob("*")
                if path.is_file() and ".git" not in path.relative_to(directory).parts
                and "__pycache__" not in path.relative_to(directory).parts}
    if files(source) != files(TARGET):
        raise RuntimeError("Local component file list differs from pinned source")
    for relative in files(source):
        if str(relative) != META["file"] and digest(source / relative) != digest(TARGET / relative):
            raise RuntimeError(f"Local component has an additional modification: {relative}")
    print("Project-local lwIP patch verified; global ESP-IDF untouched")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
