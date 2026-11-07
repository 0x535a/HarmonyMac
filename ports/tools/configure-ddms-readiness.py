#!/usr/bin/env python3
"""Plan/apply original DeviceManager readiness before DDMS in an offline virtual root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('root', type=Path)
p.add_argument('--helper', type=Path, required=True, help='OHOS ABI sa-service-ready executable')
p.add_argument('--apply', action='store_true')
a = p.parse_args()
root = a.root.resolve()
mount = subprocess.check_output(['findmnt', '-n', '-o', 'UUID,SOURCE,TARGET', '--target', str(root)], text=True).split()
assert len(mount) == 3 and mount[0] == '9d776a48-2355-4d23-8890-b506ba38b113'
assert mount[1].startswith('/dev/nbd') and Path(mount[2]).resolve() == root
assert (root / 'boot/aurora-migration/local-virtual-only').is_file()
helper = a.helper.resolve()
data = helper.read_bytes()
assert data[:6] == b'\x7fELF\x02\x01' and int.from_bytes(data[18:20], 'little') == 183
path = root / 'system/etc/init.cfg'
config = json.loads(path.read_text())
services = {s['name']: s for s in config['services']}
assert {'distributeddata', 'softbus_server', 'device_manager'} <= services.keys()
service = services['distributeddata']
assert service['path'] == ['/system/bin/sa_main', '/system/profile/distributeddata.json']
assert service['start-mode'] == 'condition'
backup = root / 'boot/aurora-migration/before-ddms-readiness'
assert not backup.exists(), 'Use a fresh overlay or inspect the existing update backup'
script = Path(__file__).resolve().parents[1] / 'tests/distributeddata-ready-start.sh'
assert script.is_file()
record = {'original_path': service['path'], 'required_actual_sa': 4802,
          'maximum_wait_seconds': 60, 'helper_sha256': hashlib.sha256(data).hexdigest(),
          'scope': 'Original UID/permissions/profile retained; no simulated service or readiness',
          'apply': a.apply}
print(json.dumps(record, indent=2))
if not a.apply:
    raise SystemExit(0)
tools = root / 'system/bin/aurora'
assert not (tools / 'distributeddata-ready-start.sh').exists()
backup.mkdir()
shutil.copy2(path, backup / 'init.cfg')
shutil.copy2(helper, tools / 'sa-service-ready')
shutil.copy2(script, tools / script.name)
(tools / 'sa-service-ready').chmod(0o755)
(tools / script.name).chmod(0o755)
service['path'] = ['/system/bin/sh', '/system/bin/aurora/distributeddata-ready-start.sh']
path.write_text(json.dumps(config, indent=2) + '\n')
(backup / 'update.json').write_text(json.dumps(record, indent=2) + '\n')
subprocess.run(['sync', '-f', str(root)], check=True)
