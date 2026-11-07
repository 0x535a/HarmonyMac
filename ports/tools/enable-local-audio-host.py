#!/usr/bin/env python3
"""Enable the original primary audio HDI in the explicit local virtual root."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('root', type=Path)
p.add_argument('hc_gen', type=Path)
a = p.parse_args()
r = a.root.resolve()
assert subprocess.check_output(
    ['findmnt', '-n', '-o', 'UUID', '--target', str(r)], text=True
).strip() == '9d776a48-2355-4d23-8890-b506ba38b113'
assert (r / 'boot/aurora-migration/local-virtual-only').is_file()
config_path = r / 'system/etc/init.cfg'
config = json.loads(config_path.read_text())
assert not any(s['name'] == 'audio_host' for s in config['services'])
names = {s['name'] for s in config['services']}
assert {'allocator_host', 'composer_host', 'audio_server'} <= names
hcs = Path(__file__).resolve().parents[1] / 'config/hdf-graphics-audio-test.hcs'
hcb = r / 'vendor/etc/hdfconfig/hdf_default.hcb.aurora'
record = r / 'boot/aurora-migration/before-audio-host'
record.mkdir(exist_ok=False)
(record / 'init.cfg').write_bytes(config_path.read_bytes())
(record / 'hdf_default.hcb').write_bytes(hcb.read_bytes())
subprocess.run([str(a.hc_gen), '-o', str(hcb), str(hcs)], check=True)
# Host indices follow the allocator/composer/audio order in this HCS.
config['services'].append(dict(
    name='audio_host',
    path=['/vendor/bin/hdf_devhost', '-i', '2', '-n', 'audio_host', '-p', '0', '-s', '0'],
    uid='audio_host', gid=['audio_host', 'audio', 'system'],
    caps=['DAC_OVERRIDE', 'DAC_READ_SEARCH', 'SYS_NICE'],
    secon='u:r:audio_host:s0', disabled=1, once=1, sandbox=0, console=1,
    **{'start-mode': 'condition'},
))
job = next(j for j in config['jobs'] if j['name'] == 'post-init')
job['cmds'].insert(job['cmds'].index('start audio_server') + 1, 'start audio_host')
config_path.write_text(json.dumps(config, indent=2) + '\n')
print('Enabled original primary audio HDI; ALSA backend and hardware remain unqualified')
