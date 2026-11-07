#!/usr/bin/env python3
"""Expose an OHOS LLVM installation to Meson without executing target binaries.

The metadata provider is Debian's matching 19.1.7 llvm-config. Headers/library
paths, shared linking and RTTI are taken from the target build, never glibc.
"""
import os
from pathlib import Path
import subprocess
import sys

prefix = Path(os.environ.get('OHOS_LLVM_PREFIX', '/root/harmonyos-work/llvm19-ohos-install')).resolve()
args = sys.argv[1:]
if not (prefix / 'include/llvm/Config/llvm-config.h').is_file():
    raise SystemExit(f'Missing target LLVM headers: {prefix}')
version = subprocess.check_output(['/usr/bin/llvm-config-19', '--version'], text=True).strip()
if version != '19.1.7':
    raise SystemExit(f'Expected native LLVM metadata version 19.1.7, got {version}')
if '--has-rtti' in args:
    print('NO')
elif '--shared-mode' in args:
    print('shared')
elif '--libfiles' in args:
    print(prefix / 'lib/libLLVM.so.19.1')
elif '--libs' in args:
    # Meson queries --libs and --ldflags together. Preserve the target search
    # directory instead of accidentally resolving a native system LLVM.
    print((f'-L{prefix / "lib"} ' if '--ldflags' in args else '') + '-l:libLLVM.so.19.1')
elif '--system-libs' in args:
    print('')
else:
    value = subprocess.check_output(['/usr/bin/llvm-config-19', *args], text=True)
    value = value.replace('/usr/lib/llvm-19', str(prefix))
    print(value.rstrip())
