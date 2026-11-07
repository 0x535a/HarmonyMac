#!/usr/bin/env python3
"""Build a native test7 initramfs from matched modules, static tools and audited firmware. No devices accessed."""
import argparse, gzip, hashlib, io, json, os, re, shutil, stat, tarfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--modules',type=Path,required=True)
p.add_argument('--bootstrap',type=Path,required=True)
p.add_argument('--firmware',type=Path,required=True)
p.add_argument('--ext4-uuid',type=Path,required=True,help='Static ext4 UUID reader; minimal BusyBox does not contain ext volume detection')
p.add_argument('--kernel-log',type=Path,required=True)
p.add_argument('--fat-uuid',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
release='7.2.11-aurora-hmos-j514s-test7'
source=a.modules/'lib/modules'/release
assert source.is_dir() and not a.output.exists()
assert not a.output.resolve().is_relative_to('/dev')
deps={}
for line in (source/'modules.dep').read_text().splitlines():
    name, values=line.split(':',1);deps[name]=values.split()
byname={Path(k).stem.replace('-','_'):k for k in deps}
builtins={Path(x).stem.replace('-','_') for x in (source/'modules.builtin').read_text().splitlines()}
seed='pinctrl-apple-gpio gpio-shared-proxy spi-apple i2c-pasemi-platform spmi-apple-controller apple_nvmem_spmi macsmc gpio-macsmc sn201202x tps6598x phy-apple-atc dwc3-apple thunderbolt_apple onboard_usb_dev mux-apple-display-crossbar phy-apple-dptx typec_displayport apple-sart nvme-apple apple-rtkit-helper apple-sio apple-dockchannel dockchannel-hid spi-hid-apple-of hid-apple hid-magicmouse hid-multitouch macsmc-input apple-pmp appledrm'.split()
soft={}
for line in (source/'modules.softdep').read_text().splitlines():
    bits=line.split()
    if bits and bits[0]=='softdep':soft[bits[1].replace('-','_')]=[x for x in bits[2:] if x not in ['pre:','post:']]
selected=set();missing=[]
def add(name):
    norm=name.replace('-','_')
    if norm in builtins:return
    if norm not in byname:
        missing.append(name);return
    path=byname[norm]
    if path in selected:return
    selected.add(path)
    for dep in deps[path]:add(Path(dep).stem)
    for dep in soft.get(norm,[]):add(dep)
for name in seed:add(name)
assert not missing, f'Missing module requirements: {missing}'
a.output.mkdir(parents=True);root=a.output/'root';root.mkdir()
for name in ['bin','dev','proc','sys','run','newroot','etc','lib/firmware']:(root/name).mkdir(parents=True,exist_ok=True)
for name in ['init','bin/busybox','bin/binder-setup','bin/input-device-permissions']:
    f=a.bootstrap/name
    assert f.read_bytes()[:4]==b'\x7fELF',name
    shutil.copyfile(f,root/name);(root/name).chmod(0o755)
for name in 'sh [ mount mkdir cat grep echo sleep test switch_root modprobe insmod blkid find readlink uname ls chown chmod sync poweroff setsid dmesg'.split():
    (root/'bin'/name).symlink_to('busybox')
assert a.ext4_uuid.read_bytes()[:4] == b'\x7fELF'
shutil.copyfile(a.ext4_uuid,root/'bin/ext4-uuid');(root/'bin/ext4-uuid').chmod(0o755)
for name,src in [('kernel-log',a.kernel_log),('fat-uuid',a.fat_uuid)]:
    assert src.read_bytes()[:4] == b'\x7fELF'
    shutil.copyfile(src,root/'bin'/name);(root/'bin'/name).chmod(0o755)
shutil.copyfile(Path(__file__).resolve().parents[1]/'initramfs/native-log-collect',root/'bin/native-log-collect')
(root/'bin/native-log-collect').chmod(0o755)
(root/'bin/dmesg').unlink();(root/'bin/dmesg').symlink_to('kernel-log')
shutil.copyfile(Path(__file__).resolve().parents[1]/'initramfs/native-stage-init',root/'aurora-init');(root/'aurora-init').chmod(0o755)
dest=root/'lib/modules'/release;dest.mkdir(parents=True)
for path in sorted(selected):
    f=source/path
    assert release.encode()+b' ' in f.read_bytes(),f'Vermagic mismatch {path}'
    target=dest/path;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,target)
for name in ['modules.builtin','modules.builtin.modinfo','modules.softdep']:
    shutil.copyfile(source/name,dest/name)
(dest/'modules.alias').write_text('\n'.join(line for line in (source/'modules.alias').read_text().splitlines() if line.startswith('alias ') and line.split()[-1].replace('-','_') in {Path(k).stem.replace('-','_') for k in selected})+'\n')
ash=a.bootstrap/'bin/hmos_ashmem.ko';assert release.encode()+b' ' in ash.read_bytes()
shutil.copyfile(ash,dest/'hmos_ashmem.ko')
(dest/'modules.dep').write_text('\n'.join(k+': '+ ' '.join(deps[k]) for k in sorted(selected))+'\nhmos_ashmem.ko:\n')
# This static BusyBox was built with empty DEFAULT_MODULES_DIR/DEPMOD_FILE.
# Generate an explicit dependency/alias loader, without invoking its modprobe.
loader=['#!/bin/sh', 'export PATH=/bin', 'case "$1" in']
all_deps={k:deps[k] for k in selected};all_deps['hmos_ashmem.ko']=[]
aliases={Path(k).stem.replace('-','_'):[] for k in all_deps}
for line in (dest/'modules.alias').read_text().splitlines():
    bits=line.split()
    if len(bits)==3:aliases[bits[2].replace('-','_')].append(bits[1])
def ordered(path, seen):
    if path in seen:return []
    seen.add(path);result=[]
    for dep in all_deps[path]:result+=ordered(dep,seen)
    return result+[path]
for path in sorted(all_deps):
    name=Path(path).stem;norm=name.replace('-','_')
    patterns=list(dict.fromkeys([name,norm]+aliases[norm]))
    loader.append('    '+'|'.join(patterns)+')')
    for dependency in ordered(path,set()):
        module=Path(dependency).stem.replace('-','_')
        loader.append(f'        if ! /bin/grep -q "^{module} " /proc/modules; then')
        loader.append(f'            /bin/insmod /lib/modules/{release}/{dependency} || exit 1')
        loader.append('        fi')
    loader.append('        exit 0;;')
loader += ['    *) exit 1;;','esac','']
(root/'bin/modprobe').unlink()
(root/'bin/modprobe').write_text('\n'.join(loader));(root/'bin/modprobe').chmod(0o755)
with tarfile.open(a.firmware) as archive:
    # Firmware tar contains paths relative to /lib/firmware, not a root filesystem.
    archive.extractall(root/'lib/firmware',filter='data')
# Portable newc writer: root ownership and Unix modes independent of host uid.
archive=io.BytesIO();ino=1
for f in [root]+sorted(root.rglob('*')):
    name='.' if f==root else './'+str(f.relative_to(root));st=f.lstat()
    if f.is_symlink():mode=stat.S_IFLNK|0o777;data=os.readlink(f).encode()
    elif f.is_dir():mode=stat.S_IFDIR|0o755;data=b''
    else:mode=stat.S_IFREG|(0o755 if str(f.relative_to(root)) in ['init','aurora-init','bin/busybox','bin/binder-setup','bin/input-device-permissions','bin/ext4-uuid','bin/modprobe','bin/kernel-log','bin/fat-uuid','bin/native-log-collect'] else 0o644);data=f.read_bytes()
    nameb=name.encode()+b'\0';fields=[ino,mode,0,0,1,0,len(data),0,0,0,0,len(nameb),0]
    archive.write(b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+nameb)
    archive.write(b'\0'*(-archive.tell()%4));archive.write(data);archive.write(b'\0'*(-archive.tell()%4));ino+=1
name=b'TRAILER!!!\0';fields=[0,0,0,0,1,0,0,0,0,0,0,len(name),0]
archive.write(b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+name);archive.write(b'\0'*(-archive.tell()%512))
image=a.output/'native-initrd.img';image.write_bytes(gzip.compress(archive.getvalue(),compresslevel=6,mtime=0))
manifest={'kernel_release':release,'root_uuid':'de5b04b6-597d-4382-b6a3-9e04388ccd6d','default_mode':'readonly rescue','module_count':len(selected)+1,'modules':sorted(selected)+['hmos_ashmem.ko'],'firmware_source_sha256':hashlib.file_digest(a.firmware.open('rb'),'sha256').hexdigest(),'image_sha256':hashlib.file_digest(image.open('rb'),'sha256').hexdigest(),'image_bytes':image.stat().st_size,'native_boot_verified':False}
(a.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
