#!/usr/bin/env python3
"""Read ELF64 program headers without executing source binaries."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def inspect(path):
    with path.open('rb') as source:
        head = source.read(64)
        if head[:4] != b'\x7fELF':
            return None
        if len(head) != 64 or head[4:6] != b'\x02\x01':
            return {'unsupported_ident': head[:16].hex()}
        fields = struct.unpack('<16sHHIQQQIHHHHHH', head)
        kind, machine, entry, offset, item_size, count = (
            fields[1], fields[2], fields[4], fields[5], fields[9], fields[10])
        if (count and item_size < 56) or count > 65535:
            raise ValueError(f'invalid program headers: {path}')
        loads, interpreter = [], None
        for index in range(count):
            source.seek(offset + index * item_size)
            row = source.read(56)
            if len(row) != 56:
                raise ValueError(f'truncated program header: {path}')
            ptype, flags, poff, vaddr, _, filesz, memsz, align = struct.unpack('<IIQQQQQQ', row)
            if ptype == 1:
                loads.append(dict(offset=poff, vaddr=vaddr, filesz=filesz,
                                  memsz=memsz, flags=flags, align=align))
            if ptype == 3:
                source.seek(poff)
                interpreter = source.read(filesz).rstrip(b'\0').decode(errors='replace')
        page_conflicts = []
        for left, right in zip(sorted(loads, key=lambda row: row['vaddr']),
                               sorted(loads, key=lambda row: row['vaddr'])[1:]):
            if ((left['vaddr'] + left['memsz'] + 16383) // 16384 > right['vaddr'] // 16384
                    and left['flags'] != right['flags']):
                page_conflicts.append([left['flags'], right['flags']])
        return dict(type=kind, machine=machine, entry=entry, interpreter=interpreter,
                    loads=loads,
                    incongruent_loads_16k=sum((row['vaddr'] - row['offset']) % 16384 != 0
                                               for row in loads),
                    mixed_permissions_same_16k_page=page_conflicts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records, errors, cores = [], [], []
    core_names = {'init', 'appspawn', 'nwebspawn', 'foundation', 'render_service',
                  'ark_js_vm', 'ark', 'libc.so', 'ld-musl-aarch64.so.1',
                  'multimodalinput', 'libmmi-server.z.so', 'libark_jsruntime.so',
                  'libEGL.so', 'libGLESv2.so', 'libace.z.so'}
    for volume in ('system', 'vendor', 'sys_prod', 'chip_prod'):
        root = args.source / volume
        for parent, _, names in os.walk(root, followlinks=False):
            for name in names:
                path = Path(parent) / name
                if path.is_symlink() or not path.is_file():
                    continue
                try:
                    result = inspect(path)
                    if result is None:
                        continue
                    result['path'] = str(path.relative_to(args.source))
                    result['size'] = path.stat().st_size
                    records.append(result)
                    if name in core_names:
                        with path.open('rb') as core:
                            result['sha256'] = hashlib.file_digest(core, 'sha256').hexdigest()
                        dynamic = subprocess.run(['readelf', '-dW', str(path)],
                                                 capture_output=True, text=True)
                        result['dynamic'] = dynamic.stdout
                        cores.append(result)
                except (OSError, ValueError, struct.error) as error:
                    errors.append(dict(path=str(path), error=str(error)))
        print(f'{volume}: {len(records)} ELF files accumulated', flush=True)
    summary = dict(
        elf_files=len(records), core_files=len(cores), errors=errors,
        machine_counts=dict(collections.Counter(row.get('machine', 'unsupported') for row in records)),
        files_with_incongruent_loads_16k=sum(bool(row.get('incongruent_loads_16k')) for row in records),
        files_with_mixed_permissions_same_16k_page=sum(bool(row.get('mixed_permissions_same_16k_page'))
                                                      for row in records),
        interpreters=dict(collections.Counter(row.get('interpreter') for row in records)))
    for name, value in [('elf-records.json', records), ('elf-core.json', cores), ('elf-summary.json', summary)]:
        (args.output / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
