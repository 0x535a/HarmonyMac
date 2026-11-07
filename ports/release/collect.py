#!/usr/bin/env python3
"""Archive one local VM run's logs, reports, and screenshots."""
import argparse
from pathlib import Path
import tarfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('run',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
assert a.run.is_dir() and not a.output.exists()
with tarfile.open(a.output,'x:gz') as archive:
 for path in sorted(a.run.iterdir()):
  if path.is_file() and not path.is_symlink() and path.suffix in ['.log','.json','.jsonl','.png','.ppm','.txt']:
   archive.add(path,arcname=a.run.name+'/'+path.name,recursive=False)
print(a.output)
