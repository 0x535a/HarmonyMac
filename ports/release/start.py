#!/usr/bin/env python3
"""Start the local OHOS development image with a persistent disposable overlay."""
import argparse
import json
import platform
from pathlib import Path
import subprocess
import sys
import uuid
from datetime import datetime, timezone

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--dry-run',action='store_true')
p.add_argument('--memory-mib',type=int,default=4096)
p.add_argument('--cpus',type=int,default=4)
p.add_argument('--timeout',type=int,default=7200)
p.add_argument('--window',action='store_true',help='Show the virtual desktop in a native macOS QEMU window')
a=p.parse_args();root=Path(__file__).resolve().parent
if sys.platform!='darwin' or platform.machine()!='arm64':p.error('This launcher requires macOS on Apple Silicon')
manifest=json.loads((root/'manifest.json').read_text())
runtime=root/'runtime';disk=runtime/'system.qcow2'
run=root/'runs'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
cmd=[sys.executable,str(root/'tools/run-service-vm.py'),str(root/'kernel/Image'),
     str(root/'boot/initrd.img'),str(run),'--disk',str(disk),'--memory-mib',str(a.memory_mib),
     '--cpus',str(a.cpus),'--display-width','1920','--display-height','1200',
     '--virtio-input','--input-events','--virtio-console','--user-network',
     '--append-extra','audit=0 loglevel=3 aurora.diagport=1','--timeout',str(a.timeout),
     '--require','LOCAL_ROOT_PID1=/system/bin/init','--require','MMI_INPUT_EVENTS_PASS',
     '--require','NATIVE_VSYNC_PASS','--require','INTERACTIVE_CHECK_END',
     '--capture-marker','OOBE_LANGUAGE_CONTINUE_CAPTURE']
if manifest.get('boot_mode') == 'direct-desktop':
 cmd=[sys.executable,str(root/'tools/run-service-vm.py'),str(root/'kernel/Image'),
      str(root/'boot/initrd.img'),str(run),'--disk',str(disk),'--memory-mib',str(a.memory_mib),
      '--cpus',str(a.cpus),'--display-width','1920','--display-height','1200',
      '--virtio-input','--virtio-console','--user-network',
      '--append-extra','audit=0 loglevel=3 aurora.diagport=1','--timeout',str(a.timeout),
      '--require','LOCAL_ROOT_PID1=/system/bin/init','--require','NATIVE_VSYNC_PASS',
      '--require','INTERACTIVE_CHECK_END','--capture-marker','DESKTOP_DIRECT95_READY']
if a.window:
 cmd+=['--display','cocoa']
if a.dry_run:
 print(json.dumps({'create_overlay':not disk.exists(),'command':cmd,
                   'persistent_virtual_identity':bool(manifest.get('virtual_hardware_identity_required')),
                   'qualification':manifest['qualification']},indent=2));raise SystemExit(0)
runtime.mkdir(exist_ok=True)
if not disk.exists():
 subprocess.run(['qemu-img','create','-f','qcow2','-F','qcow2','-b','../rootfs.qcow2','system.qcow2'],cwd=runtime,check=True)
 (runtime/'managed-overlay.json').write_text(json.dumps({'base':'../rootfs.qcow2','created_by':'aurora-local-preview'},indent=2)+'\n')
else:
 marker=json.loads((runtime/'managed-overlay.json').read_text())
 assert marker['created_by']=='aurora-local-preview'
 info=json.loads(subprocess.check_output(['qemu-img','info','--output=json',str(disk)],text=True))
 assert info['format']=='qcow2' and Path(info['full-backing-filename']).resolve()==(root/'rootfs.qcow2').resolve()
if manifest.get('virtual_hardware_identity_required'):
 identity_path=runtime/'virtual-machine.json'
 if identity_path.exists():
  assert not identity_path.is_symlink()
  identity=json.loads(identity_path.read_text())
  machine_uuid=uuid.UUID(identity['uuid'])
  assert machine_uuid.int!=0 and identity['created_by']=='aurora-local-preview'
 else:
  machine_uuid=uuid.UUID(manifest['default_machine_uuid']) if manifest.get('default_machine_uuid') else uuid.uuid4()
  with identity_path.open('x') as stream:
   json.dump({'uuid':str(machine_uuid),'created_by':'aurora-local-preview',
              'scope':'This VM virtual hardware identity; no vendor attestation'},stream,indent=2)
   stream.write('\n')
 cmd+=['--machine-uuid',str(machine_uuid)]
print('Local development preview. Qualification:',manifest['qualification'],flush=True)
print('Run evidence:',run,flush=True)
raise SystemExit(subprocess.call(cmd))
