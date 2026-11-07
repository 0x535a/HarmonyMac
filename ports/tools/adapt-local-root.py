#!/usr/bin/env python3
"""Prepare the explicit local test image for independent OHOS boot; never a physical disk."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
p.add_argument('work',type=Path)
a=p.parse_args();r=a.root.resolve();w=a.work.resolve()
uuid='9d776a48-2355-4d23-8890-b506ba38b113'
assert subprocess.check_output(['findmnt','-n','-o','UUID','--target',str(r)],text=True).strip()==uuid
assert subprocess.check_output(['findmnt','-n','-o','SOURCE','--target',str(r)],text=True).strip()=='/dev/nbd4'
assert (r/'.aurora-rootfs-populated').is_file()
record=r/'boot/aurora-migration';original=record/'local-original'
assert not original.exists(),'Root already adapted; use saved originals for explicit subsequent changes'
original.mkdir()
fixture=w/'render-service-test1/root/testroot'
for path in ['system/etc/init.cfg','system/etc/init','vendor/etc/init','sys_prod/etc/init',
             'chip_prod/etc/init','system/lib64/init','system/etc/ld-musl-namespace-aarch64.ini']:
    src=r/path
    if not src.exists():continue
    dst=original/path;dst.parent.mkdir(parents=True,exist_ok=True)
    src.rename(dst)
    if dst.is_dir():src.mkdir()

def copy_tree(path):
    subprocess.run(['rsync','-aHAX','--numeric-ids',str(fixture/path)+'/',str(r/path)+'/'],check=True)
for path in ['system/lib64/aurora-mesa','vendor/lib64/aurora-graphics','system/bin/aurora']:
    (r/path).mkdir(parents=True,exist_ok=True);copy_tree(path)
for name in ['libdisplay_buffer_vdi_impl.z.so','libdisplay_composer_vdi_impl.z.so']:
    src=r/'vendor/lib64'/name;dst=original/'vendor/lib64'/name;dst.parent.mkdir(parents=True,exist_ok=True)
    if src.exists() or src.is_symlink():src.rename(dst)
    shutil.copy2(w/'aurora-graphics-build'/name,src)
for name,target in [('libEGL.so','/vendor/lib64/aurora-graphics/libEGL_impl.so'),
                    ('libGLESv2.so','/system/lib64/aurora-mesa/libGLESv2.so.2'),
                    ('libGLESv3.so','/system/lib64/aurora-mesa/libGLESv2.so.2')]:
    src=r/'system/lib64'/name;dst=original/'system/lib64'/name;dst.parent.mkdir(parents=True,exist_ok=True)
    if src.exists() or src.is_symlink():src.rename(dst)
    src.symlink_to(target)
shutil.copy2(fixture/'system/etc/ld-musl-namespace-aarch64.ini',r/'system/etc/ld-musl-namespace-aarch64.ini')
shutil.copy2(fixture/'vendor/etc/hdfconfig/hdf_default.hcb',r/'vendor/etc/hdfconfig/hdf_default.hcb.aurora')
# The ISO vendor path is a root-absolute symlink; replace only that link,
# preserving its original target and the original chip_prod HCB.
hcb=r/'vendor/etc/hdfconfig/hdf_default.hcb'
(original/'hdf_default_link.json').write_text(json.dumps({'target':os.readlink(hcb) if hcb.is_symlink() else None})+'\n')
if not hcb.is_symlink():shutil.copy2(hcb,original/'hdf_default.hcb')
hcb.unlink();hcb.symlink_to('hdf_default.hcb.aurora')
# Keep generic init support; the original UDID hook consumes this VM's boot
# serial. Platform-specific autorun modules remain in the migration backup.
for name in ['libselinuxadp.z.so','libudidmodule.z.so']:
    shutil.copy2(original/'system/lib64/init'/name,r/'system/lib64/init'/name)
config=json.loads((fixture/'system/etc/init.cfg').read_text())
for job in config['jobs']:
    job['cmds']=[('mkdir /data/service/el0/access_token 0750 root access_token'
                 if cmd.startswith('mkdir /data/service/el0/access_token ') else cmd)
                for cmd in job.get('cmds',[])]
config['services']=[s for s in config['services'] if s['name']!='aurora-service-test']
config['services'].append(dict(name='aurora-boot-check',
    path=['/system/bin/sh','/system/bin/aurora/local-boot-check.sh'],
    uid='root',gid=['root'],disabled=1,once=1,sandbox=0,
    caps=['SYS_BOOT','SYS_ADMIN','SYS_PTRACE','DAC_OVERRIDE','DAC_READ_SEARCH']))
config['services'].append(dict(name='aurora-hilog-capture',
    path=['/system/bin/sh','-c','exec /system/bin/aurora/hilog-sockets dump >/data/boot/aurora/hilograw.log 2>&1'],
    uid='root',gid=['root'],disabled=1,once=1,sandbox=0))
for service in config['services']:
    service['start-mode']='condition'
config['jobs'][1]['cmds']=[s for s in config['jobs'][1]['cmds'] if s!='start aurora-service-test']+['start aurora-boot-check']
config['jobs'][1]['cmds'][:0]=['start aurora-hilog-capture','syncexec /system/bin/aurora/busybox sleep 1']
config['jobs'][0]['cmds']+=['setparam persist.sys.graphic.GpuApitype 0']
(r/'system/etc/init.cfg').write_text(json.dumps(config,indent=2)+'\n')
selinux=r/'system/etc/selinux/config'
shutil.copy2(selinux,original/'selinux-config')
selinux.write_text('SELINUX=permissive\n')
for path in ['tmp','data/boot/aurora','data/samgr','data/service/el0/access_token',
             'data/service/el0/startup/init','data/service/el0/render_service','data/system/RTShaderCache']:
    (r/path).mkdir(parents=True,exist_ok=True)
os.chown(r/'data/samgr',5555,5555);os.chmod(r/'data/samgr',0o740)
(record/'local-virtual-only').write_text(uuid+'\n')
(record/'local-adaptation.json').write_text(json.dumps({'uuid':uuid,'virtual_only':True,
    'original_configs':str(original.relative_to(r)),'services':[s['name'] for s in config['services']],
    'qualified_native_boot':False},indent=2)+'\n')
print('Prepared local-image controlled service boot; original files preserved at',original)
