#!/usr/bin/env python3
"""Convert this Mac's read-only wireless firmware into Linux loader filenames.

This creates only a local staging directory. It never runs an installer,
changes firmware on the Mac, configures its radios or writes a physical disk.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path, help='new output directory for this Mac')
p.add_argument('--firmware-root', type=Path, default=Path('/usr/share/firmware'),
               help='directory containing the original wifi/ and bluetooth/ inputs')
a = p.parse_args()
out = a.output.resolve()
firmware_root = a.firmware_root.resolve()
if out.exists() or out.is_relative_to(firmware_root) or out.is_relative_to('/dev'):
    p.error('output must be a new directory outside the firmware and device trees')
vendor = Path(__file__).resolve().parents[1] / 'firmware/asahi_vendorfw'
source=json.loads((vendor/'SOURCE.json').read_text())
for item in source['files']:
    assert hashlib.sha256((vendor/item['path']).read_bytes()).hexdigest()==item['sha256']
sys.dont_write_bytecode = True
sys.path.insert(0,str(vendor.parent))
from asahi_vendorfw.wifi import WiFiFWCollection
from asahi_vendorfw.bluetooth import BluetoothFWCollection
roots=[('wifi',firmware_root / 'wifi',WiFiFWCollection),
       ('bluetooth',firmware_root / 'bluetooth',BluetoothFWCollection)]
assert all(path.is_dir() for _,path,_ in roots)
out.mkdir(parents=True);dedup={};files=[]
for kind,path,collection in roots:
    for name,firmware in sorted(collection(str(path)).files()):
        rel=Path(name);assert rel.parts[0]=='brcm' and '..' not in rel.parts and not rel.is_absolute()
        original=path/firmware.name;assert original.resolve().is_relative_to(path)
        original_stat=original.stat();data=original.read_bytes()
        digest=hashlib.sha256(firmware.data).hexdigest();assert digest==firmware.sha
        target=out/rel;target.parent.mkdir(parents=True,exist_ok=True);assert not target.exists()
        linked=dedup.get(digest)
        if linked:os.link(linked,target)
        else:target.write_bytes(firmware.data);target.chmod(0o644);dedup[digest]=target
        assert hashlib.sha256(target.read_bytes()).hexdigest()==digest
        after=original.stat();assert (original_stat.st_ino,original_stat.st_size,original_stat.st_mtime_ns)==(after.st_ino,after.st_size,after.st_mtime_ns)
        files.append(dict(path=name,kind=kind,source=str(original),source_sha256=hashlib.sha256(data).hexdigest(),
            sha256=digest,bytes=len(firmware.data),hardlink_to=str(linked.relative_to(out)) if linked else None,
            transformed_nvram=(kind=='wifi' and rel.suffix=='.txt' and digest!=hashlib.sha256(data).hexdigest())))
assert any(f['kind']=='wifi' and '4388' in f['path'] and f['path'].endswith('.bin') for f in files)
assert any(f['kind']=='bluetooth' and f['path'].endswith('.ptb') for f in files)
record=dict(scope='Local-only firmware preparation from existing macOS wireless files; no hardware loading or RF qualification',
    converter=source,source_roots=[str(path) for _,path,_ in roots],source_modified=False,
    files=files,total_logical_bytes=sum(f['bytes'] for f in files),unique_payload_bytes=sum(p.stat().st_size for p in dedup.values()),
    wifi_file_count=sum(f['kind']=='wifi' for f in files),bluetooth_file_count=sum(f['kind']=='bluetooth' for f in files),
    physical_hardware_verified=False,installed_into_candidate=False)
(out/'manifest.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({k:v for k,v in record.items() if k not in ['files','converter']},indent=2))
