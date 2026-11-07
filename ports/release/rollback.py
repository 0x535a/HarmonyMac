#!/usr/bin/env python3
"""Archive a stopped managed runtime overlay and create a clean one from the preview base."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apply',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parent;runtime=root/'runtime';disk=runtime/'system.qcow2'
marker=json.loads((runtime/'managed-overlay.json').read_text())
assert marker['created_by']=='aurora-local-preview' and disk.is_file() and not disk.is_symlink()
info=json.loads(subprocess.check_output(['qemu-img','info','--output=json',str(disk)],text=True))
assert info['format']=='qcow2' and Path(info['full-backing-filename']).resolve()==(root/'rootfs.qcow2').resolve()
opened=subprocess.run(['lsof','-t','--',str(disk)],capture_output=True,text=True)
assert not opened.stdout.strip(),'Stop the VM before rollback'
backup=runtime/('system-before-rollback-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')+'.qcow2')
print('Archive',disk,'to',backup)
if a.apply:
 disk.rename(backup)
 try:subprocess.run(['qemu-img','create','-f','qcow2','-F','qcow2','-b','../rootfs.qcow2','system.qcow2'],cwd=runtime,check=True)
 except BaseException:
  if disk.exists():disk.unlink()
  backup.rename(disk);raise
 print('Clean runtime created; previous data remain in the archived overlay.')
else:print('Preview only; use --apply to perform the reversible rollback.')
