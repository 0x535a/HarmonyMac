#!/usr/bin/env python3
"""Copy an original OHOS ELF dependency closure into a disposable test root."""
import argparse
import collections
import json
import posixpath
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('output', type=Path)
p.add_argument('executables', nargs='+')
a = p.parse_args()
if a.output.exists():
    raise SystemExit('Output must be new')
a.output.mkdir(parents=True)


def physical(guest):
    parts = PurePosixPath(guest).parts
    if len(parts) > 1 and parts[1] in {'vendor', 'sys_prod', 'chip_prod'}:
        return a.source.joinpath(parts[1], *parts[2:])
    return a.source.joinpath('system', *parts[1:])


def resolve(guest):
    pending = list(PurePosixPath(guest).parts[1:])
    done = []
    links = 0
    while pending:
        part = pending.pop(0)
        if part == '..':
            if done:
                done.pop()
            continue
        if part in {'.', ''}:
            continue
        candidate = physical('/' + '/'.join(done + [part]))
        if candidate.is_symlink():
            links += 1
            if links > 40:
                raise ValueError('Symlink loop: ' + guest)
            target = PurePosixPath(candidate.readlink().as_posix())
            if target.is_absolute():
                done = []
                pending = list(target.parts[1:]) + pending
            else:
                pending = list(target.parts) + pending
        else:
            if not candidate.exists():
                return None
            done.append(part)
    result = physical('/' + '/'.join(done))
    return result if result.is_file() else None


paths = (a.source / 'system/system/etc/ld-musl-aarch64.path').read_text().strip().split(':')
namespace = (a.source / 'system/system/etc/ld-musl-namespace-aarch64.ini').read_text()
for row in re.findall(r'namespace\.default\.lib\.paths\s*=\s*(.*)', namespace):
    paths.extend(part for part in row.split(':') if part not in paths)
queue = collections.deque(a.executables)
copied, missing = {}, []
while queue:
    guest = posixpath.normpath(queue.popleft())
    if guest in copied:
        continue
    src = resolve(guest)
    if src is None:
        missing.append(guest)
        continue
    dest = a.output / guest.lstrip('/')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    dest.chmod(src.stat().st_mode & 0o777)
    copied[guest] = str(src)
    header = subprocess.run(['readelf', '-lW', str(src)], capture_output=True, text=True)
    match = re.search(r'Requesting program interpreter: ([^\]]+)', header.stdout)
    if match:
        queue.append(match.group(1))
    dynamic = subprocess.run(['readelf', '-dW', str(src)], capture_output=True, text=True)
    for needed in re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic.stdout):
        found = next((d + '/' + needed for d in paths if resolve(d + '/' + needed)), None)
        if found:
            queue.append(found)
        else:
            missing.append(guest + ': ' + needed)
for name in ['ld-musl-aarch64.path', 'ld-musl-namespace-aarch64.ini']:
    src = a.source / 'system/system/etc' / name
    dest = a.output / 'system/etc' / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    if src.is_file():
        shutil.copyfile(src, dest)
for name in ['tmp', 'data', 'dev', 'proc', 'sys']:
    (a.output / name).mkdir(exist_ok=True)
(a.output / 'etc').symlink_to('system/etc')
manifest = {'executables': a.executables, 'copied': copied, 'missing': missing,
            'bytes': sum((a.output / s.lstrip('/')).stat().st_size for s in copied)}
(a.output / 'closure-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps({'files': len(copied), 'bytes': manifest['bytes'], 'missing': missing}, indent=2))
if missing:
    raise SystemExit(2)
