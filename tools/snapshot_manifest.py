"""Create or verify a local snapshot's file hashes; never flash or read NVS."""
import argparse
import hashlib
import json
from pathlib import Path

MANIFEST = 'manifest.json'


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('Snapshot contains a symlink')
        if path.is_file() and path != root / MANIFEST:
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(65536), b''):
                    digest.update(chunk)
            result[path.relative_to(root).as_posix()] = {
                'bytes': path.stat().st_size, 'sha256': digest.hexdigest()}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['create', 'check'])
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    if args.snapshot.is_symlink() or not args.snapshot.is_dir():
        parser.error('Snapshot must be an existing directory, not a symlink')
    root = args.snapshot.resolve()
    manifest = root / MANIFEST
    try:
        files = inventory(root)
        if args.action == 'create':
            if manifest.exists():
                raise ValueError('Manifest already exists; refusing to replace it')
            if not files:
                raise ValueError('Snapshot is empty')
            with manifest.open('x') as stream:
                json.dump({'version': 1, 'files': files}, stream, indent=2)
                stream.write('\n')
        else:
            saved = json.loads(manifest.read_text())
            if saved.get('version') != 1 or saved.get('files') != files:
                raise ValueError('Snapshot file list, size or hash differs')
    except (OSError, ValueError) as exc:
        parser.exit(1, f'FAIL: {exc}\n')
    print(f'PASS: {args.action}; {len(files)} files')


if __name__ == '__main__':
    main()
