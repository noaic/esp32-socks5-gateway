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


def lf_bytes(data):
    # Git for Windows can check out text as CRLF even at the pinned commit.
    # Do not strip whitespace, a BOM, or lone CR bytes: those are real changes.
    return data.replace(b"\r\n", b"\n")


def digest(path, normalize=False):
    data = path.read_bytes()
    return hashlib.sha256(lf_bytes(data) if normalize else data).hexdigest()


def local_ip4(directory):
    path = directory / META["file"]
    # Never normalize/write through a local symlink into the SDK or elsewhere.
    for candidate in (path, *path.parents):
        if candidate == ROOT:
            break
        if candidate.is_symlink():
            raise RuntimeError(f"Refusing to touch a local symlink: {candidate}")
    return path


def state(directory):
    value = digest(local_ip4(directory), normalize=True)
    if value == META["original_sha256"]:
        return "original"
    if value == META["patched_sha256"]:
        return "patched"
    raise RuntimeError("Local ip4.c has changes beyond LF/CRLF line endings; "
                       "refusing to overwrite it. Back up and review the local override")


def require_state(directory, expected):
    if state(directory) != expected:
        raise RuntimeError(f"Local ip4.c must be in the {expected} state")


def normalize_local(directory):
    path = local_ip4(directory)
    data = path.read_bytes()
    normalized = lf_bytes(data)
    if data != normalized:
        path.write_bytes(normalized)


def apply_patch(directory, reverse=False):
    args = ["git", "apply"] + (["--reverse"] if reverse else [])
    path = local_ip4(directory)
    # Stage only ip4.c so a patch failure cannot partially change an existing
    # override. Normalize patch input too; never rewrite the tracked patch.
    with tempfile.TemporaryDirectory(prefix="lwip-patch-") as temporary:
        staged = Path(temporary)
        staged_file = staged / META["file"]
        staged_file.parent.mkdir(parents=True)
        staged_file.write_bytes(lf_bytes(path.read_bytes()))
        patch = lf_bytes(PATCH.read_bytes())
        subprocess.run([*args, "--check"], input=patch, cwd=staged, check=True)
        subprocess.run(args, input=patch, cwd=staged, check=True)
        require_state(staged, "original" if reverse else "patched")
        # git apply itself can emit CRLF under the user's Git configuration.
        path.write_bytes(lf_bytes(staged_file.read_bytes()))


def verify_component(source, target):
    # Other files remain byte-for-byte copies, including binary files.
    def files(directory):
        return {path.relative_to(directory) for path in directory.rglob("*")
                if path.is_file() and ".git" not in path.relative_to(directory).parts
                and "__pycache__" not in path.relative_to(directory).parts}
    source_files = files(source)
    if source_files != files(target):
        raise RuntimeError("Local component file list differs from pinned source; "
                           "back up and review the local override")
    for relative in source_files:
        # Compare Path objects: str(relative) uses backslashes on Windows.
        if relative != Path(META["file"]) and digest(source / relative) != digest(target / relative):
            raise RuntimeError(f"Local component has an additional modification: {relative}; "
                               "back up and review the local override")


def verify_source(idf, source):
    """Check tracked component bytes against HEAD, not another mutable copy."""
    expected = set()
    def tree(repository, scope, prefix):
        command = ["git", "ls-tree", "-rz", "HEAD"]
        if scope:
            command += ["--", scope]
        records = subprocess.check_output(command, cwd=repository).split(b"\0")
        blobs = []
        for record in records:
            if not record:
                continue
            info, raw_path = record.split(b"\t", 1)
            mode, kind, object_id = info.split()
            path = Path(os.fsdecode(raw_path))
            relative = prefix / (path.relative_to(scope) if scope else path)
            if kind == b"commit":
                if relative != Path("lwip") or object_id.decode() != META["lwip_commit"]:
                    raise RuntimeError(f"Unexpected SDK submodule: {relative}")
                continue
            if kind != b"blob" or mode == b"120000":
                raise RuntimeError(f"Unsupported SDK file type: {relative}")
            expected.add(relative)
            blobs.append((object_id, repository / path, relative))
        if not blobs:
            raise RuntimeError("SDK component has no tracked source files")
        output = subprocess.check_output(["git", "cat-file", "--batch"], cwd=repository,
                                         input=b"".join(object_id+b"\n" for object_id, _, _ in blobs))
        offset = 0
        for _, path, relative in blobs:
            end = output.index(b"\n", offset)
            size = int(output[offset:end].split()[-1]); offset = end+1
            baseline = output[offset:offset+size]; offset += size+1
            if not path.is_file() or path.is_symlink():
                raise RuntimeError(f"SDK source missing or symlinked: {relative}")
            actual = path.read_bytes()
            # Accept only CRLF/LF checkout differences for text, never binary
            # modifications, whitespace edits, BOM or lone CR differences.
            if b"\0" not in baseline:
                actual, baseline = lf_bytes(actual), lf_bytes(baseline)
            if actual != baseline:
                raise RuntimeError(f"Global SDK component has an uncommitted modification: {relative}; "
                                   "back up and review the SDK; nothing was overwritten")
    tree(idf, "components/lwip", Path())
    tree(source / "lwip", None, Path("lwip"))
    actual = {path.relative_to(source) for path in source.rglob("*")
              if path.is_file() and ".git" not in path.relative_to(source).parts
              and "__pycache__" not in path.relative_to(source).parts}
    if actual != expected:
        raise RuntimeError("SDK component contains missing or untracked files; back up and review it")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("apply", "check", "restore"))
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    local_ip4(TARGET)
    if args.action == "restore":
        if not TARGET.exists():
            print("No project-local override to restore")
            return
        if state(TARGET) == "patched":
            apply_patch(TARGET, reverse=True)
        else:
            normalize_local(TARGET)
        require_state(TARGET, "original")
        print("Restored project-local ip4.c; baseline requires PoC disabled; reapply before menuconfig if still enabled")
        return
    if not args.idf_path:
        raise RuntimeError("Activate ESP-IDF first, or provide --idf-path")
    idf = Path(args.idf_path).resolve()
    source = idf / "components/lwip"
    idf_commit = run("git", "rev-parse", "HEAD", cwd=idf)
    if idf_commit != META["idf_commit"]:
        raise RuntimeError(f"ESP-IDF revision mismatch: expected {META['idf_commit']}, got {idf_commit}; "
                           "use the pinned SDK or review/rebase the patch")
    lwip_commit = run("git", "rev-parse", "HEAD", cwd=source / "lwip")
    if lwip_commit != META["lwip_commit"]:
        raise RuntimeError(f"esp-lwIP revision mismatch: expected {META['lwip_commit']}, got {lwip_commit}")
    if digest(source / META["file"], normalize=True) != META["original_sha256"]:
        raise RuntimeError("Global IDF ip4.c has changes beyond LF/CRLF line endings; "
                           "refusing to copy. Review the SDK file; the script has not modified it")
    verify_source(idf, source)
    if not TARGET.exists():
        if args.action == "check":
            raise RuntimeError("Override missing; run prepare_lwip.py apply")
        TARGET.parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="lwip-stage-", dir=TARGET.parent) as temporary:
            staged = Path(temporary) / "lwip"
            shutil.copytree(source, staged, ignore=shutil.ignore_patterns(".git", "__pycache__"))
            apply_patch(staged)
            require_state(staged, "patched")
            verify_component(source, staged)
            staged.rename(TARGET)
    else:
        current = state(TARGET)
        verify_component(source, TARGET)
        if current == "original":
            if args.action == "check":
                raise RuntimeError("Override restored; run prepare_lwip.py apply")
            apply_patch(TARGET)
        elif args.action == "apply":
            normalize_local(TARGET)
    require_state(TARGET, "patched")
    print("Project-local lwIP patch verified; global ESP-IDF untouched")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
