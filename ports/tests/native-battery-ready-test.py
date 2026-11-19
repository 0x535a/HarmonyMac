#!/usr/bin/env python3
"""Check invalid/late battery states cannot reach original HDI initialization."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import threading

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--busybox', type=Path)
parser.add_argument('--helper', type=Path, default=Path(__file__).resolve().parents[1]/'initramfs/native-battery-ready')
args = parser.parse_args()
helper = args.helper
shell = [str(args.busybox), 'sh'] if args.busybox else ['sh']
with tempfile.TemporaryDirectory(prefix='native-battery-ready-') as tmp:
    root = Path(tmp)
    def put(device, **fields):
        path = root/device
        path.mkdir(exist_ok=True)
        for name, value in fields.items():
            (path/name).write_text(str(value)+'\n')
    def check(expected, attempts='1'):
        result = subprocess.run(shell+[str(helper), str(root), attempts],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == expected, result.stdout+result.stderr
        assert ('HMOS_NATIVE_BATTERY_READY path=' in result.stdout) == expected
        return result.stdout
    check(False)  # Missing hardware is not zero capacity.
    put('hid', type='Battery', scope='Device', present=1, capacity=100, status='Full')
    check(False)  # A peripheral cannot satisfy laptop battery readiness.
    put('apple', type='Battery', scope='System', present=1, capacity=100, status='Full')
    assert 'capacity=100' in check(True)
    for invalid in ('', 'junk', '-1', '101', '99999999999999999999'):
        put('apple', capacity=invalid)
        check(False)
    put('apple', capacity=0)
    assert 'capacity=0' in check(True)  # Real empty battery still reaches normal protection.
    put('apple', present=0)
    check(False)  # The exact failed callback state must not be accepted.
    put('apple', present=1, status='Unknown')
    check(False)
    put('apple', status='Charging', capacity=100)
    put('second', type='Battery', scope='System', present=1, capacity=100, status='Full')
    check(False)  # Ambiguous system supply is an error.
    for file in (root/'second').iterdir():
        file.unlink()
    (root/'second').rmdir()
    put('apple', present=0)
    timer = threading.Timer(0.2, lambda: put('apple', present=1))
    timer.start()
    check(True, '3')  # Driver initialization completing after the first read.
    timer.join()
print('NATIVE_BATTERY_READY_TEST_PASS')
