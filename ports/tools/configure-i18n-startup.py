#!/usr/bin/env python3
"""Plan/apply original i18n startup before Foundation configuration requests."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('root', type=Path)
p.add_argument('--apply', action='store_true')
a = p.parse_args()
root = a.root.resolve()
mount = subprocess.check_output(['findmnt', '-n', '-o', 'UUID,SOURCE,TARGET', '--target', str(root)], text=True).split()
assert len(mount) == 3 and mount[0] == '9d776a48-2355-4d23-8890-b506ba38b113'
assert mount[1].startswith('/dev/nbd') and Path(mount[2]).resolve() == root
assert (root / 'boot/aurora-migration/local-virtual-only').is_file()
path = root / 'system/etc/init.cfg'
config = json.loads(path.read_text())
service = next(s for s in config['services'] if s['name'] == 'i18n_service')
assert service['path'] == ['/system/bin/sa_main', '/system/profile/i18n_service.json']
assert service['start-mode'] == 'condition'
profile_path = root / 'system/profile/i18n_service.json'
profile = json.loads(profile_path.read_text())
ability = next(s for s in profile['systemability'] if s['name'] == 1602)
assert ability['run-on-create'] is False
jobs = [j for j in config['jobs'] if 'start foundation' in j.get('cmds', [])]
assert len(jobs) == 1
job = jobs[0]
assert job.get('condition') == 'aurora.boot.hdi_ready=true'
assert job['cmds'].count('start i18n_service') == 1
backup = root / 'boot/aurora-migration/before-i18n-startup'
target = root / 'system/profile/aurora-i18n_service.json'
assert not backup.exists() and not target.exists(), 'Use a fresh overlay or inspect the existing update'
record = {'original_profile': str(profile_path.relative_to(root)), 'eager_original_ability': 1602,
          'startup_job': job['name'], 'insert_before': 'foundation and its app configuration requests',
          'scope': 'Original binary, permissions and idle unload retained; no language/provisioning values changed',
          'apply': a.apply}
print(json.dumps(record, indent=2))
if not a.apply:
    raise SystemExit(0)
backup.mkdir()
shutil.copy2(path, backup / 'init.cfg')
shutil.copy2(profile_path, backup / 'i18n_service.json')
ability['run-on-create'] = True
target.write_text(json.dumps(profile, indent=2) + '\n')
shutil.copystat(profile_path, target)
shutil.chown(target, user=profile_path.stat().st_uid, group=profile_path.stat().st_gid)
service['path'] = ['/system/bin/sa_main', '/system/profile/' + target.name]
job['cmds'].remove('start i18n_service')
job['cmds'].insert(1, 'start i18n_service')
path.write_text(json.dumps(config, indent=2) + '\n')
(backup / 'update.json').write_text(json.dumps(record, indent=2) + '\n')
subprocess.run(['sync', '-f', str(root)], check=True)
