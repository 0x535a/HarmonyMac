#!/usr/bin/env python3
"""Send QMP pointer events and capture the real virtual scanout."""
import argparse
import json
from pathlib import Path
import socket
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--click', nargs=2, type=int, metavar=('X', 'Y'))
p.add_argument('--move-relative', nargs=2, type=int, metavar=('DX', 'DY'),
               help='Move the real relative mouse; OHOS acceleration applies, so deltas are not screen coordinates')
p.add_argument('--scroll', type=int, metavar='TICKS', help='Relative mouse wheel ticks: positive down, negative up')
p.add_argument('--button', choices=['left', 'right', 'middle'], help='Press/release at the current pointer position')
p.add_argument('--capture', type=Path)
p.add_argument('--delay', type=float, default=1)
a = p.parse_args()
if a.click and (a.move_relative or a.scroll is not None or a.button):
    p.error('Choose absolute --click or relative pointer actions')
if a.move_relative and any(abs(v) > 32767 for v in a.move_relative):
    p.error('Relative deltas must be between -32767 and 32767')
if a.scroll is not None and abs(a.scroll) > 100:
    p.error('Scroll must be between -100 and 100 ticks')
c = json.loads((a.output / 'qmp.json').read_text())
if (a.move_relative or a.scroll is not None or a.button) and c.get('absolute_pointer'):
    p.error('Relative controls require a VM without an absolute tablet; OHOS tablet mapping is unverified')
if a.click:
    assert c['absolute_pointer'], 'Click requires an absolute pointer'
    assert c.get('absolute_pointer_verified',False), 'OHOS tablet mapping is unverified; use original UITest through guest-console for coordinate input'
    x, y = a.click
    assert 0 <= x < c['display_width'] and 0 <= y < c['display_height']
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
    connection.settimeout(8)
    connection.connect(c['socket'])
    stream = connection.makefile('rwb', buffering=0)
    assert 'QMP' in json.loads(stream.readline())
    def request(value):
        stream.write((json.dumps(value) + '\n').encode())
        while True:
            reply = json.loads(stream.readline())
            if 'error' in reply:
                raise RuntimeError(reply)
            if 'return' in reply:
                return reply['return']
    request({'execute': 'qmp_capabilities'})
    if a.click:
        for events in [
            [{'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / (c['display_width'] - 1))}},
             {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / (c['display_height'] - 1))}}],
            [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}],
            [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}],
        ]:
            request({'execute': 'input-send-event', 'arguments': {'events': events}})
            time.sleep(0.08)
        with (a.output / 'pointer-events.jsonl').open('a') as log:
            log.write(json.dumps({'time': time.time(), 'click': a.click}) + '\n')
    if a.move_relative:
        request({'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': a.move_relative[0]}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': a.move_relative[1]}}]}})
        time.sleep(0.15)
    buttons = ([('wheel-down' if a.scroll > 0 else 'wheel-up')] * abs(a.scroll)
               if a.scroll else []) + ([a.button] if a.button else [])
    for button in buttons:
        for down in [True, False]:
            request({'execute': 'input-send-event', 'arguments': {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': down}}]}})
            time.sleep(0.08)
    if a.move_relative or buttons:
        with (a.output / 'pointer-events.jsonl').open('a') as log:
            log.write(json.dumps({'time': time.time(), 'relative_delta': a.move_relative,
                                  'scroll_ticks': a.scroll, 'button': a.button,
                                  'scope': 'Real QMP relative events; screen position depends on OHOS pointer acceleration'}) + '\n')
    if a.capture:
        time.sleep(a.delay)
        request({'execute': 'screendump', 'arguments': {'filename': str(a.capture.resolve())}})
        print(a.capture)
