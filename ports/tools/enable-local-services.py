#!/usr/bin/env python3
"""Add original OHOS service definitions to the explicit virtual root image."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
p.add_argument('services',nargs='+',choices=['accesstoken_service','foundation','appspawn','multimodalinput','accountmgr','huks_service','hidumper_service','installs','distributeddata','storage_manager','storage_daemon','faultloggerd','audio_server','av_session','device_manager','softbus_server','deviceinfoservice','pasteboard_service','time_service','inputmethod_service','wifi_manager_service','bluetooth_service','locationhub','media_service','accessibility','powermgr','aps_manager','i18n_service','netmanager','netsysnative'])
a=p.parse_args();r=a.root.resolve()
assert subprocess.check_output(['findmnt','-n','-o','UUID','--target',str(r)],text=True).strip()=='9d776a48-2355-4d23-8890-b506ba38b113'
assert (r/'boot/aurora-migration/local-virtual-only').is_file()
original=r/'boot/aurora-migration/local-original/system/etc/init'
config_path=r/'system/etc/init.cfg';c=json.loads(config_path.read_text())
files={'accesstoken_service':'access_token.cfg','foundation':'foundation.cfg','appspawn':'appspawn.cfg',
       'multimodalinput':'multimodalinput.cfg','accountmgr':'accountmgr.cfg','huks_service':'huks_service.cfg',
       'hidumper_service':'hidumper_service.cfg','installs':'installs.cfg',
       'distributeddata':'distributed_data.cfg','storage_manager':'storage_manager.cfg',
       'storage_daemon':'storage_daemon.cfg','faultloggerd':'faultloggerd.cfg','audio_server':'audio_server.cfg',
       'av_session':'avsession_service.cfg','device_manager':'device_manager.cfg','softbus_server':'softbus_server_musl.cfg','deviceinfoservice':'deviceinfoservice.cfg','pasteboard_service':'pasteboardservice.cfg',
       'time_service':'timeservice.cfg','inputmethod_service':'inputmethodservice.cfg',
       'wifi_manager_service':'wifi_standard.cfg','bluetooth_service':'bluetooth_service.cfg',
       'locationhub':'locationsa.cfg','media_service':'media_service.cfg','accessibility':'accessibility.cfg','powermgr':'powermgr.cfg','aps_manager':'aps_manager.cfg','i18n_service':'i18n_service.cfg','netmanager':'netmanager_base.cfg','netsysnative':'netsysnative.cfg'}
existing={s['name'] for s in c['services']}
assert not existing.intersection(a.services),'Requested service already enabled'
changes=[];pre=[]
for name in a.services:
    cfg=json.loads((original/files[name]).read_text())
    s=next(s for s in cfg['services'] if s['name']==name)
    # Keep credentials, permissions, sockets and domains. Kirin scheduler
    # cgroups and whole-system reboot loops are outside this virtual test.
    removed={k:s.pop(k) for k in ['critical','writepid','start-mode','cgroup','ondemand'] if k in s}
    s.update(disabled=1,once=1,sandbox=0,console=1)
    s['start-mode']='condition'
    if name=='device_manager' and 'start-on-demand' in s:
        removed['start-on-demand']=s.pop('start-on-demand')
    if name=='softbus_server':
        # The ISO adds an L410 BLE response shim. The virtual dependency test
        # starts the original SoftBus service without that platform preload.
        env=s.get('env',[])
        removed_env=[e for e in env if e.get('name')=='LD_PRELOAD'
                     and e.get('value')=='/system/lib64/l410-collaboration/libsoftbus_ble_pc_rsp_preload_v1.so']
        if removed_env:
            removed['l410_ble_preload']=removed_env
            s['env']=[e for e in env if e not in removed_env]
    if name=='appspawn':
        # Stock appspawn reserves argv storage for renamed child processes;
        # its main requires at least APP_LEN_PROC_NAME (256) writable bytes.
        # init also limits each individual path argument to 200 bytes.
        s['path']=['/system/bin/appspawn','-mode','appspawn',
                   '--startup-padding-1='+'0'*140,'--startup-padding-2='+'0'*140]
    if name=='foundation':s['path']=['/system/bin/sa_main','/system/profile/foundation.json']
    if name in ['wifi_manager_service','locationhub']:
        # Use retained original abilities for virtual API dependencies.
        # Wi-Fi bypasses the ISO Kirin replacement; locator starts only SA 2802.
        profile=json.loads((r/'system/profile'/f'{name}.json').read_text())
        for ability in profile['systemability']:
            if name=='wifi_manager_service' or ability['name']==2802:
                ability['run-on-create']=True
        target=r/'system/profile'/f'aurora-{name}.json'
        target.write_text(json.dumps(profile,indent=2)+'\n')
        s['path']=['/system/bin/sa_main','/system/profile/'+target.name]
    if name=='netmanager':
        pre.append('mkdir /data/service/el1/public/eth 0750 net_manager net_manager')
    c['services'].append(s)
    for job in cfg.get('jobs',[]):
        if job['name'].startswith(('services:','service:')) or job['name'] in s.get('jobs',{}).values():
            if name=='powermgr':
                # Keep original userspace state preparation, omit Kirin
                # hibernate hooks and virtual guest sysfs power writes.
                job['cmds']=[cmd for cmd in job.get('cmds',[])
                             if cmd.startswith(('mkdir /data/','chown ' ,'chmod '))
                             and '/data/' in cmd]
            if not any(j['name']==job['name'] for j in c['jobs']):c['jobs'].append(job)
        else:
            pre.extend(cmd for cmd in job.get('cmds',[]) if cmd.startswith(('mkdir ','chown ','chmod ','restorecon ','export ')))
    changes.append({'service':name,'removed_platform_fields':removed,'path':s['path']})
prejob=next(j for j in c['jobs'] if j['name']=='pre-init')
prejob['cmds'][:0]=list(dict.fromkeys(pre))
prejob['cmds']=[('mkdir /data/service/el0/access_token 0750 root access_token'
                if cmd.startswith('mkdir /data/service/el0/access_token ') else cmd)
               for cmd in prejob['cmds']]
startup_jobs=[j for j in c['jobs'] if 'start aurora-boot-check' in j.get('cmds',[])]
assert len(startup_jobs)==1,'Expected exactly one controlled core-service startup job'
postjob=startup_jobs[0]
pos=postjob['cmds'].index('start aurora-boot-check')
postjob['cmds'][pos:pos]=['start '+name for name in a.services]
check=next(s for s in c['services'] if s['name']=='aurora-boot-check')
if not any(e['name']=='HMOS_BOOT_WAIT_SECONDS' for e in check.get('env',[])):
    check.setdefault('env',[]).append(dict(name='HMOS_BOOT_WAIT_SECONDS',value='25'))
config_path.write_text(json.dumps(c,indent=2)+'\n')
(r/'boot/aurora-migration'/('services-'+('-'.join(a.services))+'.json')).write_text(json.dumps(changes,indent=2)+'\n')
print(json.dumps(changes,indent=2))
