#!/usr/bin/env python3
"""Run a diskless diagnostic guest and retain bounded, explicit evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import socket
import os
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('kernel', type=Path)
p.add_argument('initrd', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--require', action='append', required=True)
p.add_argument('--timeout', type=int, default=90)
p.add_argument('--memory-mib', type=int, default=2048, help='guest RAM; large original-service initrds need more')
p.add_argument('--cpus', type=int, default=2, help='virtual CPU count')
p.add_argument('--display', choices=['none', 'cocoa'], default='none', help='macOS window or headless diagnostic display')
p.add_argument('--display-width', type=int, default=1280, help='virtio GPU preferred mode width')
p.add_argument('--display-height', type=int, default=800, help='virtio GPU preferred mode height')
p.add_argument('--virtio-input', action='store_true', help='add virtual keyboard/mouse for OHOS MMI integration')
p.add_argument('--virtio-tablet', action='store_true', help='also add an absolute-coordinate pointer for interactive UI verification')
p.add_argument('--user-network', action='store_true', help='attach a virtio Ethernet NIC to QEMU user networking')
p.add_argument('--input-events', action='store_true', help='send key/mouse events over QMP once MMI_INPUT_READY appears')
p.add_argument('--capture-marker', action='append', default=[], help='capture scanout 1 second after this serial marker')
p.add_argument('--capture-delay', type=float, default=1.0, help='seconds between serial marker and scanout capture')
p.add_argument('--append-extra', default='', help='extra kernel cmdline args')
p.add_argument('--disk', type=Path, help='local qcow2 overlay for the controlled full-root test')
p.add_argument('--machine-uuid', help='persistent UUID of this local VM; also exposes its disk serial to original OHOS init')
p.add_argument('--serial-socket', action='store_true', help='local Unix serial console with serial.log retained for interactive diagnostics')
p.add_argument('--virtio-console', action='store_true', help='separate local diagnostic command port, without sharing the kernel UART')
a = p.parse_args()
if a.serial_socket and a.virtio_console:
    p.error('choose one interactive console transport')
if a.memory_mib < 512:
    p.error('guest memory must be at least 512 MiB')
if a.cpus<1 or a.cpus>16:
    p.error('guest CPU count must be 1..16')
if not (320 <= a.display_width <= 4096 and 240 <= a.display_height <= 4096):
    p.error('virtual display must be 320..4096 by 240..4096')
if a.disk and (not a.disk.is_file() or a.disk.suffix!='.qcow2'):
    p.error('--disk must be an existing local qcow2 file, never a device')
# Refuse a disk or backing file still opened by a file export/another guest.
# In particular, a failed guest-side image update must never fall through into
# booting the same image while qemu-nbd still owns it.
if a.disk:
    def check_image_users(path):
        opened = subprocess.run(['lsof', '-t', '--', str(path)], capture_output=True, text=True)
        if opened.stdout.strip():
            p.error(f'Image is open; stop the other guest or unmount/disconnect file NBD first: {path}')
        if opened.returncode not in (0, 1):
            p.error(f'Cannot check image users with lsof: {path}: {opened.stderr.strip()}')
    check_image_users(a.disk.resolve())
    info = subprocess.run(['qemu-img', 'info', '--backing-chain', '--output=json',
                           str(a.disk.resolve())], capture_output=True, text=True)
    if info.returncode:
        p.error(f'Cannot inspect image backing chain: {info.stderr.strip()}')
    for node in json.loads(info.stdout):
        check_image_users(Path(node['filename']).resolve())

machine_uuid = None
disk_serial = None
if a.machine_uuid:
    if not a.disk:
        p.error('--machine-uuid requires a local --disk')
    try:
        machine_uuid = uuid.UUID(a.machine_uuid)
    except ValueError:
        p.error('--machine-uuid must be a valid UUID')
    if machine_uuid.int == 0:
        p.error('--machine-uuid must not be nil')
    if 'ohos.boot.sn=' in a.append_extra:
        p.error('Do not override the serial derived from --machine-uuid')
    # virtio-blk exposes at most 20 serial bytes. Keep the kernel boot input
    # identical to the actual emulated disk serial, rather than inventing UDID.
    disk_serial = machine_uuid.hex[:20]
for artifact in [a.kernel, a.initrd]:
    if not artifact.is_file():
        p.error(f'missing artifact: {artifact}')
a.output.mkdir(parents=True, exist_ok=False)
if machine_uuid:
    (a.output / 'virtual-machine.json').write_text(json.dumps({
        'uuid': str(machine_uuid), 'disk_serial': disk_serial,
        'scope': 'Local virtual hardware identity; original init computes UDID. No vendor attestation or activation state supplied.'
    }, indent=2) + '\n')
serial = a.output.resolve() / 'serial.log'
serial_socket = Path(f'/tmp/hmos-serial-{os.getpid()}.sock')
if a.serial_socket:
    (a.output / 'console.json').write_text(json.dumps({'socket': str(serial_socket),
        'scope': 'Local diagnostic UART; commands run inside the guest'}, indent=2) + '\n')
serial_args = (['-chardev', f'socket,id=hmosserial,path={serial_socket},server=on,wait=off,logfile={serial}',
                '-serial', 'chardev:hmosserial'] if a.serial_socket else ['-serial', f'file:{serial}'])
command = ['qemu-system-aarch64', '-machine', 'virt', '-accel', 'hvf',
           '-cpu', 'host', '-m', str(a.memory_mib), '-smp', str(a.cpus), '-device',
           f'virtio-gpu-pci,xres={a.display_width},yres={a.display_height}',
           '-display', a.display, *serial_args, '-no-reboot',
           '-kernel', str(a.kernel.resolve()), '-initrd', str(a.initrd.resolve()),
           '-append', ('console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init user4k=on loglevel=6 '
                       + (f'ohos.boot.sn={disk_serial} aurora.virtual_identity=1 ' if disk_serial else '')
                       + a.append_extra).strip()]
if machine_uuid:
    command += ['-uuid', str(machine_uuid)]
if a.virtio_input:
    command += ['-device', 'virtio-keyboard-pci', '-device', 'virtio-mouse-pci']
if a.virtio_tablet:
    if not a.virtio_input:
        p.error('--virtio-tablet requires --virtio-input')
    command += ['-device', 'virtio-tablet-pci,id=ui-tablet']
if a.disk:
    if disk_serial:
        command += ['-drive', f'file={a.disk.resolve()},format=qcow2,if=none,id=hmosroot',
                    '-device', f'virtio-blk-pci,drive=hmosroot,serial={disk_serial}']
    else:
        command += ['-drive',f'file={a.disk.resolve()},format=qcow2,if=virtio']
if a.user_network:
    command += ['-netdev', 'user,id=hmosnet', '-device',
                'virtio-net-pci,netdev=hmosnet,mac=52:54:00:48:4d:01']
if a.virtio_console:
    diag_socket = Path(f'/tmp/hmos-diag-{os.getpid()}.sock')
    command += ['-device', 'virtio-serial-pci', '-chardev',
                f'socket,id=hmosdiag,path={diag_socket},server=on,wait=off',
                '-device', 'virtserialport,chardev=hmosdiag,name=aurora.diag']
    (a.output / 'console.json').write_text(json.dumps({'socket': str(diag_socket),
        'scope': 'Separate local diagnostic virtio port'}, indent=2) + '\n')
qmp_path=Path(f'/tmp/hmos-service-{os.getpid()}.sock')
if a.input_events or a.capture_marker:
    if a.input_events and not a.virtio_input:
        p.error('--input-events requires --virtio-input')
    command += ['-qmp', f'unix:{qmp_path},server=on,wait=off']
    (a.output / 'qmp.json').write_text(json.dumps({'socket': str(qmp_path),
        'display_width': a.display_width, 'display_height': a.display_height,
        'absolute_pointer': a.virtio_tablet,
        'absolute_pointer_verified': False}, indent=2) + '\n')
sent_events=False
capture_due={}
captures={}
def capture(marker):
    index=a.capture_marker.index(marker)
    path=(a.output/f'scanout-{index}.ppm').resolve()
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
        connection.settimeout(5);connection.connect(str(qmp_path))
        stream=connection.makefile('rwb',buffering=0)
        assert 'QMP' in json.loads(stream.readline())
        for request in [{'execute':'qmp_capabilities'},
                        {'execute':'screendump','arguments':{'filename':str(path)}}]:
            stream.write((json.dumps(request)+'\n').encode())
            while True:
                reply=json.loads(stream.readline())
                if 'error' in reply: raise RuntimeError(reply)
                if 'return' in reply:break
    captures[marker]=str(path)
def send_input():
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
        connection.settimeout(5);connection.connect(str(qmp_path))
        stream=connection.makefile('rwb',buffering=0)
        assert 'QMP' in json.loads(stream.readline())
        def request(value):
            stream.write((json.dumps(value)+'\n').encode())
            while True:
                reply=json.loads(stream.readline())
                if 'error' in reply: raise RuntimeError(reply)
                if 'return' in reply:return reply
        request({'execute':'qmp_capabilities'})
        events=[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'a'}}}]
        if a.virtio_tablet:
            events += [{'type':'abs','data':{'axis':'x','value':17000}},
                       {'type':'abs','data':{'axis':'y','value':17000}}]
        else:
            events += [{'type':'rel','data':{'axis':'x','value':25}},
                       {'type':'rel','data':{'axis':'y','value':15}}]
        request({'execute':'input-send-event','arguments':{'events':events}})
        time.sleep(0.15)
        request({'execute':'input-send-event','arguments':{'events':[
            {'type':'key','data':{'down':False,'key':{'type':'qcode','data':'a'}}}]}})
        (a.output/'qmp-input.json').write_text(json.dumps({'sent':True,'events':events},indent=2)+'\n')
timed_out = False
with (a.output / 'qemu.log').open('w') as output:
    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + a.timeout
    try:
        while process.poll() is None:
            log = serial.read_text(errors='replace') if serial.exists() else ''
            if a.input_events and not sent_events and 'MMI_INPUT_READY' in log:
                send_input();sent_events=True
            for marker in a.capture_marker:
                if marker in log and marker not in capture_due:
                    capture_due[marker]=time.monotonic()+a.capture_delay
                if marker in capture_due and marker not in captures and time.monotonic()>=capture_due[marker]:
                    capture(marker)
            if 'Kernel panic - not syncing' in log:
                break
            if time.monotonic() >= deadline:
                timed_out = True
                break
            time.sleep(0.2)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        qmp_path.unlink(missing_ok=True)
        if a.serial_socket:
            serial_socket.unlink(missing_ok=True)
        if a.virtio_console:
            diag_socket.unlink(missing_ok=True)
log = serial.read_text(errors='replace') if serial.exists() else ''
faults = [s for s in ['Kernel panic', 'Unable to handle kernel', 'BUG:', 'WARNING:'] if s in log]
report = dict(command=command, qemu_exit=process.returncode, timed_out=timed_out,
              verification_scope='Requested serial markers, process exit, kernel faults, and capture-file existence only; visual/application correctness needs separate verification.',
              disk=str(a.disk.resolve()) if a.disk else None,
              input_events_sent=sent_events,
              captures=captures,
              markers={s: s in log for s in a.require}, kernel_faults=faults,
              artifacts={str(f.resolve()): hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
                         for f in [a.kernel, a.initrd]})
report['passed'] = (process.returncode == 0 and not timed_out and not faults
                    and (not a.input_events or sent_events)
                    and len(captures)==len(a.capture_marker)
                    and all(report['markers'].values()))
(a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
