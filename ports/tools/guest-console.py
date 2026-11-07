#!/usr/bin/env python3
"""Run a diagnostic shell command through a local guest UART, retaining serial.log."""
import argparse
import json
from pathlib import Path
import re
import socket
import time
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path, help='run-service-vm output with console.json')
p.add_argument('command', help='shell command to run inside this diagnostic guest')
p.add_argument('--timeout', type=float, default=30)
p.add_argument('--wait-ready', type=float, default=0, help='connect early and wait this long for the guest shell marker')
p.add_argument('--expect-disconnect', action='store_true', help='allow the port to close for a shutdown command; caller must verify guest poweroff')
a = p.parse_args()
assert a.timeout > 0
startup_deadline = time.monotonic() + a.wait_ready
while not (a.output / 'console.json').is_file():
    if not a.wait_ready or time.monotonic() >= startup_deadline:
        raise SystemExit('Console metadata is not available; inspect the VM launcher')
    time.sleep(0.1)
if not a.wait_ready:
    assert 'LOCAL_ROOT_INTERACTIVE_READY' in (a.output / 'serial.log').read_text(errors='replace'), 'Guest shell is not ready'
config = json.loads((a.output / 'console.json').read_text())
marker = 'AURORA_CONSOLE_DONE_' + uuid.uuid4().hex
pattern = re.compile(rb'\r?\n' + marker.encode() + rb'=(\d+)\r?\n')
data = bytearray()
deadline = time.monotonic() + a.timeout
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
    connection.settimeout(min(a.timeout, 5))
    while True:
        try:
            connection.connect(config['socket'])
            break
        except FileNotFoundError:
            if not a.wait_ready or time.monotonic() >= startup_deadline:
                raise
            time.sleep(0.1)
    if a.wait_ready:
        ready_deadline = time.monotonic() + a.wait_ready
        while not (a.output / 'serial.log').is_file() or 'LOCAL_ROOT_INTERACTIVE_READY' not in (a.output / 'serial.log').read_text(errors='replace'):
            if time.monotonic() >= ready_deadline:
                raise SystemExit('Guest shell readiness timeout; inspect serial.log')
            time.sleep(0.25)
        deadline = time.monotonic() + a.timeout
    connection.sendall((a.command + '\necho ' + marker + '=$?\n').encode())
    while time.monotonic() < deadline:
        connection.settimeout(min(1, max(0.01, deadline - time.monotonic())))
        try:
            chunk = connection.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            if a.expect_disconnect:
                print(data.decode(errors='replace')[-65536:])
                raise SystemExit(0)
            raise SystemExit('Guest UART closed before command completion; inspect serial.log')
        data.extend(chunk)
        match = pattern.search(data)
        if match:
            print(data[:match.start()].decode(errors='replace')[-65536:])
            raise SystemExit(int(match.group(1)))
        if len(data) > 1048576:
            del data[:-524288]
raise SystemExit('Command observation timed out; it may still be running in the guest')
