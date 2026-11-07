#!/usr/bin/env python3
"""Rebase original .hmos-data label sidecar onto the file-level /data tree."""
import argparse
import csv
import json
import os
from pathlib import Path, PurePosixPath
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('target', type=Path)
p.add_argument('labels', type=Path)
p.add_argument('--record', type=Path, required=True)
p.add_argument('--expected-uuid', default='de5b04b6-597d-4382-b6a3-9e04388ccd6d',
               help='explicit filesystem identity; local test images use a separate UUID')
a = p.parse_args()
a.target = a.target.resolve()
identity = subprocess.check_output(['findmnt', '-n', '-o', 'UUID', '--target', str(a.target)], text=True).strip()
if identity != a.expected_uuid:
    raise SystemExit('Unexpected target filesystem')


def parent_safe_path(guest):
    pending = list(PurePosixPath(guest).parts[1:])
    done, links = [], 0
    while pending:
        part = pending.pop(0)
        if part in {'', '.'}:
            continue
        if part == '..':
            if not done:
                raise ValueError('Escape above target root')
            done.pop()
            continue
        candidate = a.target.joinpath(*done, part)
        if pending and candidate.is_symlink():
            links += 1
            if links > 40:
                raise ValueError('Too many parent symlinks')
            link = PurePosixPath(os.readlink(candidate))
            if link.is_absolute():
                done = []
                pending = list(link.parts[1:]) + pending
            else:
                pending = list(link.parts) + pending
        else:
            done.append(part)
    return a.target.joinpath(*done)


result = dict(applied=0, excluded_optional_linux=0, missing=[], errors=[])
with a.labels.open(newline='') as source:
    for row in csv.DictReader(source, delimiter='\t'):
        path = row['path']
        if path == '.hmos-data':
            guest = '/data'
        elif path.startswith('.hmos-data/'):
            guest = '/data/' + path[len('.hmos-data/'):]
        else:
            result['errors'].append({'path': path, 'error': 'Unexpected source prefix'})
            continue
        if guest == '/data/vendor/l410-linux/rootfs-bookworm' or guest.startswith('/data/vendor/l410-linux/rootfs-bookworm/'):
            result['excluded_optional_linux'] += 1
            continue
        target = parent_safe_path(guest)
        if not os.path.lexists(target):
            result['missing'].append(guest)
            continue
        label = row['selinux'].encode()
        try:
            os.setxattr(target, 'security.selinux', label, follow_symlinks=False)
            assert os.getxattr(target, 'security.selinux', follow_symlinks=False) == label
            result['applied'] += 1
        except (OSError, AssertionError) as error:
            result['errors'].append({'path': guest, 'error': str(error)})
a.record.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: len(value) if isinstance(value, list) else value for key, value in result.items()}, indent=2))
if result['missing'] or result['errors']:
    raise SystemExit(2)
