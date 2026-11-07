#!/usr/bin/env python3
"""Apply the verified event-startup payload to an offline local preview overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    root, payload = args.root.resolve(), args.payload.resolve()
    manifest = json.loads((payload / 'manifest.json').read_text())
    for name, expected in manifest['files'].items():
        path = (payload / name).resolve()
        if not path.is_relative_to(payload) or not path.is_file() or digest(path) != expected:
            parser.error(f'Payload hash mismatch or invalid path: {name}')
    mount = subprocess.check_output(
        ['findmnt', '-n', '-o', 'UUID,SOURCE,TARGET', '--target', str(root)], text=True
    ).strip().split()
    if len(mount) != 3 or mount[0] != manifest['root_uuid'] or not mount[1].startswith('/dev/nbd') or Path(mount[2]).resolve() != root:
        parser.error('Expected the exact local preview ext4 mount on a file-backed NBD device')
    if root == Path('/') or not (root / 'boot/aurora-migration/local-virtual-only').is_file():
        parser.error('Missing local virtual-root marker')
    config = root / 'system/etc/init.cfg'
    if digest(config) != manifest['base_init_sha256']:
        parser.error('This update requires the unchanged 20261104-local-preview init.cfg')
    tools = root / 'system/bin/aurora'
    backup = root / 'boot/aurora-migration/before-event-startup-update'
    event_backup = root / 'boot/aurora-migration/before-event-startup'
    if backup.exists() or event_backup.exists() or (tools / 'hdi-service-ready').exists():
        parser.error('An earlier update or probe is already present; use a fresh overlay')
    for name in ['event-deadline.sh', 'event-rescue.sh']:
        if (tools / name).exists():
            parser.error(f'Unexpected existing file: {name}')
    print(json.dumps({'root': str(root), 'backup': str(backup),
                     'replace': ['system/bin/aurora/hilog-sockets'],
                     'install': ['system/bin/aurora/hdi-service-ready', 'event-driven init jobs',
                                 'bounded failure rescue'],
                     'qualification': manifest['qualification'], 'apply': args.apply}, indent=2))
    if not args.apply:
        return
    backup.mkdir()
    shutil.copy2(tools / 'hilog-sockets', backup / 'hilog-sockets')
    shutil.copy2(config, backup / 'init.cfg')
    shutil.copy2(tools / 'local-boot-check.sh', backup / 'local-boot-check.sh')
    try:
        for name in ['hilog-sockets', 'hdi-service-ready']:
            shutil.copy2(payload / 'bin' / name, tools / name)
            (tools / name).chmod(0o755)
        subprocess.run([sys.executable, str(payload / 'configure-event-startup.py'),
                        str(root), '--apply'], check=True)
    except BaseException:
        shutil.copy2(backup / 'hilog-sockets', tools / 'hilog-sockets')
        shutil.copy2(backup / 'init.cfg', config)
        shutil.copy2(backup / 'local-boot-check.sh', tools / 'local-boot-check.sh')
        for name in ['hdi-service-ready', 'event-deadline.sh', 'event-rescue.sh']:
            (tools / name).unlink(missing_ok=True)
        print('Restored original runtime files after update failure; backup retained.', file=sys.stderr)
        raise
    (backup / 'applied-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    subprocess.run(['sync', '-f', str(root)], check=True)


if __name__ == '__main__':
    main()
