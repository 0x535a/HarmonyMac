#!/usr/bin/env python3
"""Build an independent, explicitly unqualified local VM preview bundle."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import uuid

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root-image',type=Path,required=True)
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--initrd',type=Path,required=True)
p.add_argument('--kernel-package',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--kernel-profile',choices=['test6','test7'],default='test6',
               help='Only a separately verified fixed kernel/initrd/module tuple')
p.add_argument('--renderer',choices=['softpipe','llvmpipe'],default='softpipe')
p.add_argument('--direct-desktop',action='store_true',help='Package the separately verified OOBE-bypass candidate; retain explicit acceptance scope')
p.add_argument('--config-evidence',type=Path,help='Current offline config snapshot to overlay on baseline evidence')
p.add_argument('--llvmpipe-evidence',type=Path,help='External llvmpipe evidence directory (required for llvmpipe)')
p.add_argument('--activation-evidence',type=Path,help='External activation review directory (required for llvmpipe)')
p.add_argument('--extra-evidence',action='append',default=[],help='Additional named run directory under --evidence')
a=p.parse_args()
if a.renderer=='llvmpipe':
    for name in ['llvmpipe_evidence','activation_evidence']:
        value=getattr(a,name)
        if value is None or not value.is_dir():
            p.error('--'+name.replace('_','-')+' must be an existing directory for llvmpipe')
project=Path(__file__).resolve().parents[2]
for f in [a.root_image,a.kernel,a.initrd,a.kernel_package]:
    if not f.is_file() or f.resolve().is_relative_to('/dev'):
        p.error(f'Expected a local regular file: {f}')
chain=json.loads(subprocess.check_output(['qemu-img','info','--backing-chain','--output=json',str(a.root_image.resolve())],text=True))
for node in chain:
    path=Path(node['filename']).resolve()
    opened=subprocess.run(['lsof','-t','--',str(path)],capture_output=True,text=True)
    if opened.stdout.strip():p.error(f'Image is open; stop QEMU and unmount/disconnect file NBD first: {path}')
assert chain[0]['format']=='qcow2'
kernel_hash=hashlib.file_digest(a.kernel.open('rb'),'sha256').hexdigest()
initrd_hash=hashlib.file_digest(a.initrd.open('rb'),'sha256').hexdigest()
verified={
 'test6':('e1baaa7e59577378eda0507db08e80e1f2fe28dfadf255cec65bcc32de56cc1f',
          '9f20cf0a1149f22c1ac734b093a3d3fd18261880c153d75a442c83499e2b507d',
          'b152148190c5bec101d7975646fb742517620a145d6fda703c412399e3f9b178'),
 'test7':('cbf644041fbd0482535182af112338fdd4a9e4ee44c8b7a228489e2b2ee24192',
          'b622cdf3b55eb453003e2b566b43bea99969e6af401171ae82378e6cd5d2f21b',
          '5782a1d2660445a9a9037271bef55cad3fbf2ed291a288c05570fde2701cd113'),
}
assert kernel_hash==verified[a.kernel_profile][0],'Expected the verified kernel for selected profile'
assert initrd_hash==verified[a.kernel_profile][1],'Expected the matching verified initramfs'
assert hashlib.file_digest(a.kernel_package.open('rb'),'sha256').hexdigest()==verified[a.kernel_profile][2], 'Expected the matching verified module package'
raw_uuid=subprocess.check_output(['qemu-io','-r','-f','qcow2','-c','read -v 1128 16',str(a.root_image.resolve())],text=True)
uuid_bytes=bytearray()
for line in raw_uuid.splitlines():
    if re.match(r'^[0-9a-f]+:',line):
        uuid_bytes.extend(int(x,16)for x in line.split(':',1)[1].strip().split()[:16])
assert str(uuid.UUID(bytes=bytes(uuid_bytes)))=='9d776a48-2355-4d23-8890-b506ba38b113','Unexpected ext4 root UUID'
assert not a.output.exists(),'Use a new bundle output directory'
out=a.output.resolve();out.mkdir(parents=True)
for name in ['kernel','boot','tools','evidence','runtime','runs']:(out/name).mkdir()
shutil.copy2(a.kernel,out/'kernel/Image')
shutil.copy2(a.initrd,out/'boot/initrd.img')
shutil.copy2(a.kernel_package,out/f'kernel/native-{a.kernel_profile}-package.tar.zst')
for name in ['start.py','stop.py','rollback.py','collect.py','README.md']:
    shutil.copy2(project/'ports/release'/name,out/name)
if a.direct_desktop:
    shutil.copy2(project/'ports/release/README-direct-desktop.md',out/'README.md')
for name in ['run-service-vm.py','guest-console.py','guest-pointer.py']:
    shutil.copy2(Path(__file__).parent/name,out/'tools'/name)
for name in ['FULL-ROOT-BOOT.md','REAL-QUEUE-INTEGRATION.md']:
    shutil.copy2(a.evidence/name,out/'evidence'/name)
shutil.copytree(a.evidence/'local-root/current-config',out/'evidence/current-config')
if a.config_evidence:
    shutil.copytree(a.config_evidence,out/'evidence/current-config',dirs_exist_ok=True)
for name in a.extra_evidence:
    assert Path(name).name==name and name not in {'.','..'}
    source=a.evidence/name
    assert source.is_dir(),source
    target=out/'evidence'/name
    target.mkdir(exist_ok=True)
    for file in source.iterdir():
        if file.is_file() and file.suffix in ['.json','.png','.txt']:
            shutil.copy2(file,target/file.name)
shutil.copytree(project/'ports',out/'source/ports',ignore=shutil.ignore_patterns('__pycache__','*.pyc','.DS_Store'))
for directory,names in {
    'local-root-resolution37':['scanout-2.png','visual-review.json','result.json'],
    'local-root-dhcp45':['activation-result.png','interaction-review.json','result.json'],
    'local-root-rswatch43':['after-privacy.png','interaction-review.json','result.json'],
}.items():
    target=out/'evidence'/directory;target.mkdir()
    for name in names:shutil.copy2(a.evidence/directory/name,target/name)
shutil.copy2(a.evidence/'local-root/environment.json',out/'evidence/environment.json')
if a.renderer=='llvmpipe':
    shutil.copytree(a.llvmpipe_evidence,out/'evidence/llvmpipe',ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    shutil.copytree(a.activation_evidence,out/'evidence/activation-review')
print('Flattening only local image files; qualification remains incomplete.',flush=True)
subprocess.run(['qemu-img','convert','-p','-O','qcow2','-c',str(a.root_image.resolve()),str(out/'rootfs.qcow2')],check=True)
image_info=json.loads(subprocess.check_output(['qemu-img','info','--output=json',str(out/'rootfs.qcow2')],text=True))
assert image_info['format']=='qcow2' and 'backing-filename' not in image_info
hashes={}
for f in sorted(out.rglob('*')):
    if f.is_file():hashes[str(f.relative_to(out))]=hashlib.file_digest(f.open('rb'),'sha256').hexdigest()
manifest={'created_at':datetime.now(timezone.utc).isoformat(),'target':'macOS Apple Silicon HVF local development preview',
 'root_uuid':'9d776a48-2355-4d23-8890-b506ba38b113','kernel_release':'7.2.11-aurora-hmos-j514s-'+a.kernel_profile,
 'virtual_hardware_identity_required':a.kernel_profile=='test7',
 'kernel_page_size':16384,'ohos_process_page_size':4096,'display':[1920,1200],
 'kernel_source_commit':'d13466ebb8b5f2e2af3d2c8359f4c96e711a5679',
 'mesa_source_commit':'7c86130d26cecd50c7af1ac01ff3fa1705d58045',
 'renderer':a.renderer,
 'renderer_evidence_note':'See bundled run evidence; rendering qualification does not imply activation or complete desktop.',
 'initrd_modules':['hmos_ashmem.ko','virtio-gpu.ko','virtio_dma_buf.ko'],
 'native_handoff_note':'External hmos_ashmem module is in boot/initrd.img; native boot configuration and firmware are not qualified by this bundle.',
 'source_image_chain':[Path(n['filename']).name for n in chain],
 'qualification':{'complete':False,'vendor_activation_verified':False,'full_desktop_verified':False,
   'basic_app_interaction_verified':False,'thirty_minute_stability_verified':False,'native_apple_verified':False,
   'known_activation_error':'socIdCert is empty; current DCM service reaches HDI but TEEC initialization fails ffff0000'},'files':hashes}
if a.direct_desktop:
    assert a.kernel_profile=='test7' and a.renderer=='llvmpipe'
    manifest['boot_mode']='direct-desktop'
    manifest['default_machine_uuid']='164537af-1f4c-4fb4-b597-a4da65b3b257'
    manifest['qualification']['acceptance_scope']='Desktop reachable and applications open windows; application internals and 30-minute interaction excluded by user.'
    # Packaging alone cannot qualify a new output. Actual bundle runtime evidence
    # must be recorded separately before this field can be considered accepted.
    manifest['qualification']['desktop_and_app_windows_verified']=False
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
hashes['manifest.json']=hashlib.file_digest((out/'manifest.json').open('rb'),'sha256').hexdigest()
(out/'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n'for name,digest in sorted(hashes.items())))
print(out,flush=True)
