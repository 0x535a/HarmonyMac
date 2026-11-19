#!/usr/bin/env python3
"""Derive an r1 boot-only fix, retaining every unrelated frozen cpio entry."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

PORTS = Path(__file__).resolve().parents[1]
RELEASE = '7.2.11-aurora-hmos-j514s-test8'
MODULE = f'lib/modules/{RELEASE}/kernel/drivers/power/supply/macsmc-power.ko'
SOURCE_SHA = '42f20b11f7c7301446c68c13de63ffa6ca794b8329bd5dd501022ec3b9bacb5e'
MODULE_SHA = 'd08695d6fdd2944e8c78a437418dd505e92ccda455ce9c76a163ca7d41ddb107'
sha = lambda data: hashlib.sha256(data).hexdigest()


def parse(data):
    entries = {}
    pos = 0
    while data[pos:pos+6] == b'070701':
        fields = [int(data[pos+6+i*8:pos+14+i*8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = data[pos+110:pos+110+namesize-1].decode()
        begin = (pos+110+namesize+3) & ~3
        content = data[begin:begin+size]
        assert len(content) == size
        pos = (begin+size+3) & ~3
        if name == 'TRAILER!!!':
            return entries
        name = name.removeprefix('./')
        assert name not in entries
        entries[name] = (fields, content)
    raise ValueError('Missing cpio trailer')


def archive(entries):
    data = bytearray()
    for i, (name, (fields, content)) in enumerate(entries.items(), 1):
        fields = list(fields)
        encoded = ('.' if name == '.' else './'+name).encode()+b'\0'
        fields[0], fields[6], fields[11] = i, len(content), len(encoded)
        data += b'070701' + b''.join(f'{v:08x}'.encode() for v in fields) + encoded
        data += bytes(-len(data) % 4)
        data += content
        data += bytes(-len(data) % 4)
    encoded = b'TRAILER!!!\0'
    fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(encoded), 0]
    data += b'070701' + b''.join(f'{v:08x}'.encode() for v in fields) + encoded
    data += bytes(-len(data) % 512)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists()
    source, module = args.source.read_bytes(), args.module.read_bytes()
    assert sha(source) == SOURCE_SHA and sha(module) == MODULE_SHA
    assert RELEASE.encode()+b' ' in module and b'depends=macsmc\0' in module
    original = parse(gzip.decompress(source))
    entries = dict(original)
    scripts = PORTS / 'initramfs'
    # The old initializer must match the audited source before these changes.
    initializer = (scripts/'native-stage-init').read_text().replace(
        '7.2.11-aurora-hmos-j514s-test7', RELEASE).replace('native-ready-test7', 'native-ready-test8')
    prior = initializer.replace('macsmc macsmc-power gpio-macsmc', 'macsmc gpio-macsmc')
    begin = prior.index('# The HDI enumerates battery paths once.')
    end = prior.index('# This file is installed only', begin)
    prior = prior[:begin] + prior[end:]
    assert prior.encode() == original['aurora-init'][1], 'Unrelated initializer change'
    entries['aurora-init'] = (original['aurora-init'][0], initializer.encode())
    entries['bin/native-log-collect'] = (original['bin/native-log-collect'][0],
                                         (scripts/'native-log-collect').read_bytes())
    newfields = [0, 0o100755, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]
    entries['bin/native-battery-ready'] = (newfields, (scripts/'native-battery-ready').read_bytes())
    # Linux's initramfs extractor needs explicit parent directories before files.
    added_dirs = set()
    for parent in reversed(Path(MODULE).parents):
        name = str(parent)
        if name == '.' or name in entries:
            continue
        directory_fields = list(newfields)
        directory_fields[1] = 0o40755
        entries[name] = (directory_fields, b'')
        added_dirs.add(name)
    fields = list(newfields)
    fields[1] = 0o100644
    entries[MODULE] = (fields, module)
    depname = f'lib/modules/{RELEASE}/modules.dep'
    entries[depname] = (original[depname][0], original[depname][1] +
        b'kernel/drivers/power/supply/macsmc-power.ko: kernel/drivers/mfd/macsmc.ko\n')
    aliasname = f'lib/modules/{RELEASE}/modules.alias'
    entries[aliasname] = (original[aliasname][0], original[aliasname][1] +
                          b'alias platform:macsmc-power macsmc_power\n')
    loader = original['bin/modprobe'][1].decode()
    addition = f'''    macsmc-power|macsmc_power|platform:macsmc-power)
        if ! /bin/grep -q '^macsmc ' /proc/modules; then
            /bin/insmod /lib/modules/{RELEASE}/kernel/drivers/mfd/macsmc.ko || exit 1
        fi
        if ! /bin/grep -q '^macsmc_power ' /proc/modules; then
            /bin/insmod /{MODULE} || exit 1
        fi
        exit 0;;
'''
    assert loader.count('    *) exit 1;;') == 1
    entries['bin/modprobe'] = (original['bin/modprobe'][0], loader.replace(
        '    *) exit 1;;', addition+'    *) exit 1;;').encode())
    allowed = {'aurora-init', 'bin/native-log-collect', 'bin/native-battery-ready',
               MODULE, depname, aliasname, 'bin/modprobe'} | added_dirs
    changed = {name for name in entries if name not in original or entries[name] != original[name]}
    assert changed == allowed
    result = gzip.compress(archive(entries), compresslevel=6, mtime=0)
    reread = parse(gzip.decompress(result))
    assert set(reread) == set(entries)
    for name, (fields, content) in entries.items():
        assert reread[name][1] == content and reread[name][0][1:6] == fields[1:6], name
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    record = dict(source_sha256=SOURCE_SHA, module_sha256=MODULE_SHA,
                  output_sha256=sha(result), output_bytes=len(result),
                  changed_entries=sorted(changed), unrelated_entry_contents_and_modes_preserved=True,
                  kernel_unchanged=True, real_battery_required_for_normal_boot=True,
                  low_battery_protection_disabled=False, physical_boot_verified=False)
    args.output.with_suffix('.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
