#!/bin/bash
# Advance the real queue fixture to init-managed services and explicit domains.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-managed-graphics-test.sh PROJECT WORK OUTPUT [MODULES]}")
work=$(realpath "${2:?}")
out=${3:?}
bash "$project/ports/tools/prepare-real-queue-egl-test.sh" "$project" "$work" "$out" "${4:-$work/kernel-test6-package}"
python3 - "$out/root/testroot" <<'PY'
from pathlib import Path
import json,sys
r=Path(sys.argv[1]); p=r/'system/etc/init.cfg'; c=json.loads(p.read_text())
def original(file,name):
    return next(s for s in json.loads(Path(file).read_text())['services'] if s['name']==name)
services=[]
for file,name in [('/mnt/harmonyos-source/system/system/etc/init/samgr_standard.cfg','samgr'),
                  ('/mnt/harmonyos-source/vendor/etc/init/hdf_devmgr.cfg','hdf_devmgr')]:
    s=original(file,name)
    # Kirin cgroups and restart/reboot policies do not belong in this VM.
    for k in ['writepid','critical','start-mode','bootevents']:s.pop(k,None)
    s.update(disabled=1,once=1,sandbox=0,console=1)
    services.append(s)
# Root DAC is retained for the test allocator's DRM access; its own native
# token and SELinux domain are exercised independently from the probe shell.
services.append(dict(name='allocator_host',
    path=['/vendor/bin/hdf_devhost','-i','0','-n','allocator_host','-p','0','-s','0'],
    uid='root',gid=['root'],secon='u:r:allocator_host:s0',disabled=1,once=0,sandbox=0,console=1,
    env=[dict(name='LD_LIBRARY_PATH',value='/vendor/lib64:/system/lib64/aurora-mesa')]))
c['services']+=services
c['jobs'][0]['cmds']+=['setrlimit RLIMIT_NICE 40 40',
                      'setrlimit RLIMIT_NOFILE 32768 32768',
                      'mkdir /data/samgr 0740 samgr samgr']
c['jobs'][1]['cmds']=['start samgr','start hdf_devmgr','start allocator_host','start aurora-service-test']
p.write_text(json.dumps(c,indent=2)+'\n')
p=r/'system/bin/aurora/service-test.sh'; s=p.read_text()
start=s.index('samgr >/tmp/samgr.log')
end=s.index('/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -tt \\\n    -e trace=ioctl,connect',start)
s=s[:start]+'sleep 8\n'+s[end:]
diagnostics='''
managed=0
for p in /proc/[0-9]*; do
    name=$(busybox cat "$p/comm" 2>/dev/null)
    case "$name" in
        samgr) expected=u:r:samgr:s0; expected_uid=5555 ;;
        hdf_devmgr) expected=u:r:hdf_devmgr:s0; expected_uid=3044 ;;
        hdf_devhost|allocator_host) expected=u:r:allocator_host:s0; expected_uid=0 ;;
        *) continue ;;
    esac
    context=$(busybox cat "$p/attr/current" 2>/dev/null)
    parent=$(busybox awk '/^PPid:/{print $2}' "$p/status")
    uid=$(busybox awk '/^Uid:/{print $3}' "$p/status")
    echo "MANAGED_SERVICE name=$name pid=${p##*/} parent=$parent euid=$uid context=$context"
    if [ "$context" = "$expected" ] && [ "$parent" = 1 ] && [ "$uid" = "$expected_uid" ]; then
        managed=$((managed+1))
    fi
done
echo "MANAGED_SERVICE_CONTEXT_COUNT=$managed"
'''
s=s.replace('echo ORIGINAL_INIT_SERVICE_END\n',diagnostics+'echo ORIGINAL_INIT_SERVICE_END\n')
p.write_text(s)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img.tmp"
mv "$out/initrd.img.tmp" "$out/initrd.img"
printf 'Prepared init-managed graphics fixture %s\n' "$out/initrd.img"
