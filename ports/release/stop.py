#!/usr/bin/env python3
"""Ask the original guest userspace to sync and power off through its diagnostic port."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
p=argparse.ArgumentParser(description=__doc__);p.add_argument('run',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(root/'tools/guest-console.py'),str(a.run),
 'echo INTERACTIVE_CHECK_END >/dev/console; busybox sync; busybox poweroff -f','--timeout','20','--expect-disconnect'])
for _ in range(100):
 log=(a.run/'serial.log').read_text(errors='replace')
 if 'reboot: Power down' in log:
  print('Guest synced and powered down.');raise SystemExit(0)
 time.sleep(.2)
raise SystemExit('Poweroff not verified; retain the live run and inspect its serial.log')
