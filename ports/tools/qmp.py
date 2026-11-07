#!/usr/bin/env python3
"""Issue one explicit QMP command to a disposable test VM."""
import argparse
import json
import socket

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('socket')
p.add_argument('command', help='JSON command object')
a = p.parse_args()
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
    conn.settimeout(15)
    conn.connect(a.socket)
    stream = conn.makefile('rwb', buffering=0)
    greeting = json.loads(stream.readline())
    if 'QMP' not in greeting:
        raise RuntimeError(greeting)
    for command in [{'execute': 'qmp_capabilities'}, json.loads(a.command)]:
        stream.write((json.dumps(command) + '\n').encode())
        while True:
            reply = json.loads(stream.readline())
            if 'error' in reply:
                raise RuntimeError(reply)
            if 'return' in reply:
                break
    print(json.dumps(reply))
