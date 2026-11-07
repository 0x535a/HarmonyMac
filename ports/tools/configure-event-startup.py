#!/usr/bin/env python3
"""Replace virtual-root startup sleeps with original init parameter jobs and observed HDI readiness."""
import argparse
import json
from pathlib import Path
import subprocess

DEADLINE_SCRIPT = r'''#!/system/bin/sh
exec >/dev/console 2>&1
sleep 90
ready=$(/system/bin/param get aurora.boot.hdi_ready 2>/dev/null | /system/bin/tr -d ' \r\n')
if [ "$ready" != true ]; then
    exec /system/bin/sh /system/bin/aurora/event-rescue.sh
fi
if /system/bin/grep -q aurora.diagport=1 /proc/cmdline; then
    sleep 150
    ready=$(/system/bin/param get aurora.boot.diagnostic_ready 2>/dev/null | /system/bin/tr -d ' \r\n')
    if [ "$ready" != true ]; then
        exec /system/bin/sh /system/bin/aurora/event-rescue.sh
    fi
fi
'''

RESCUE_SCRIPT = r'''#!/system/bin/sh
exec >/dev/console 2>&1
echo AURORA_BOOT_FAILURE_RESCUE_READY
/system/bin/param get aurora.boot.hdf_manager_ready
/system/bin/param get aurora.boot.hdi_ready
if /system/bin/grep -q aurora.diagport=1 /proc/cmdline; then
    for port in /sys/class/virtio-ports/*; do
        [ -f "$port/name" ] || continue
        [ "$(/system/bin/cat "$port/name")" = aurora.diag ] || continue
        device=/dev/${port##*/}
        if [ ! -c "$device" ]; then
            devnum=$(/system/bin/cat "$port/dev")
            /system/bin/mknod "$device" c "${devnum%:*}" "${devnum#*:}"
        fi
        if [ -c "$device" ]; then
            exec 3<>"$device"
            echo "LOCAL_ROOT_INTERACTIVE_READY rescue=$device"
            while true; do
                /system/bin/sh <&3 >&3 2>&1
                sleep 1
            done
        fi
    done
fi
/system/bin/sync
/system/bin/aurora/busybox poweroff -f
'''

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
p.add_argument('--apply',action='store_true')
a=p.parse_args();r=a.root.resolve()
assert subprocess.check_output(['findmnt','-n','-o','UUID','--target',str(r)],text=True).strip()=='9d776a48-2355-4d23-8890-b506ba38b113'
assert (r/'boot/aurora-migration/local-virtual-only').is_file()
assert (r/'system/bin/aurora/hdi-service-ready').is_file()
path=r/'system/etc/init.cfg';config=json.loads(path.read_text())
assert not any(s['name'].startswith('aurora-hdi-ready-')for s in config['services'])
post=next(j for j in config['jobs']if j['name']=='post-init')
starts=[x for x in post['cmds']if x.startswith('start ')]
early={'aurora-hilog-capture','samgr','hdf_devmgr','allocator_host','composer_host','audio_host','param_watcher','accesstoken_service'}
assert all('start '+name in starts for name in early)
assert all(x.startswith(('start ','syncexec /system/bin/aurora/busybox sleep '))for x in post['cmds'])
core=[x for x in starts if x.split(' ',1)[1] not in early]
first=['start aps_manager']
assert all(x in core for x in first)
core=first+[x for x in core if x not in first]
stages=[
 {'name':'param:aurora.boot.hilog_ready=true','condition':'aurora.boot.hilog_ready=true',
  'cmds':['write /dev/console AURORA_BOOT_STAGE=logger-ready','start samgr']},
 {'name':'param:bootevent.samgr.ready=true','condition':'bootevent.samgr.ready=true',
  'cmds':['write /dev/console AURORA_BOOT_STAGE=samgr-ready','start param_watcher','start accesstoken_service','start hdf_devmgr','start aurora-hdi-ready-manager']},
 {'name':'param:aurora.boot.hdf_manager_ready=true','condition':'aurora.boot.hdf_manager_ready=true',
  'cmds':['write /dev/console AURORA_BOOT_STAGE=hdf-manager-ready','start allocator_host','start composer_host','start audio_host','start aurora-hdi-ready-services']},
 {'name':'param:aurora.boot.hdi_ready=true','condition':'aurora.boot.hdi_ready=true',
  'cmds':['write /dev/console AURORA_BOOT_STAGE=hdi-ready']+core},
]
assert not {s['name']for s in stages}.intersection(j['name']for j in config['jobs'])
record={'replaced_post_init':post['cmds'],'stages':stages,'core_start_order':core,
        'condition_start_services':[s['name']for s in config['services']],
        'deadlines_seconds':{'hdi':90,'requested_diagnostic_channel':240},
        'scope':'Explicit local virtual root; readiness flags reflect original logger/SAMgr/HDI observations'}
print(json.dumps(record,indent=2))
if not a.apply:raise SystemExit(0)
backup=r/'boot/aurora-migration/before-event-startup';backup.mkdir(exist_ok=False)
(backup/'init.cfg').write_bytes(path.read_bytes())
boot_check=r/'system/bin/aurora/local-boot-check.sh'
(backup/'local-boot-check.sh').write_bytes(boot_check.read_bytes())
text=boot_check.read_text()
if 'aurora.boot.diagnostic_ready' not in text:
    assert 'exec 3<>"$diag_device"' in text
    text=text.replace('exec 3<>"$diag_device"','exec 3<>"$diag_device"\n                /system/bin/param set aurora.boot.diagnostic_ready true >/dev/null 2>&1')
    boot_check.write_text(text)
for name,content in [('event-deadline.sh',DEADLINE_SCRIPT),('event-rescue.sh',RESCUE_SCRIPT)]:
    script=r/'system/bin/aurora'/name;script.write_text(content);script.chmod(0o755)
logger=next(s for s in config['services']if s['name']=='aurora-hilog-capture')
logger.setdefault('env',[]).append({'name':'HMOS_LOG_READY_PARAM','value':'aurora.boot.hilog_ready'})
post['cmds']=['start aurora-hilog-capture','start aurora-boot-deadline']
for service in config['services']:
    service['start-mode']='condition'
config['jobs']+=stages
for suffix,options in [
 ('manager',['--manager-only','--wait','60','--publish','aurora.boot.hdf_manager_ready']),
 ('services',['--wait','60','--publish','aurora.boot.hdi_ready','allocator_service','display_composer_service','audio_manager_service']),
]:
 config['services'].append({'name':'aurora-hdi-ready-'+suffix,
   'path':['/system/bin/aurora/hdi-service-ready']+options,
   'uid':'root','gid':['root'],'disabled':1,'once':1,'sandbox':0,'console':1,'start-mode':'condition'})
config['services'].append({'name':'aurora-boot-deadline',
    'path':['/system/bin/sh','/system/bin/aurora/event-deadline.sh'],
    'uid':'root','gid':['root'],'disabled':1,'once':1,'sandbox':0,'console':1,'start-mode':'condition',
    'caps':['SYS_BOOT','SYS_ADMIN','MKNOD','DAC_OVERRIDE','DAC_READ_SEARCH','SYS_PTRACE','SYSLOG'],
    'permission':['ohos.permission.DUMP'],'permission_acls':['ohos.permission.DUMP']})
path.write_text(json.dumps(config,indent=2)+'\n')
(r/'boot/aurora-migration/event-startup.json').write_text(json.dumps(record,indent=2)+'\n')
