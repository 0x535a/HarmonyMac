#!/usr/bin/env python3
"""Make a local full-height appledrm experiment from the native battery-ready v3 initrd."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import tempfile


SOURCE_SHA256 = "609f93911932534db70f96fb81d7df6d956f6c1b146771fbd73cc41fe32e1b49"
RELEASE = "7.2.11-aurora-hmos-j514s-test8"
MODULE = f"lib/modules/{RELEASE}/kernel/drivers/gpu/drm/apple/appledrm.ko"
LOADER = "bin/modprobe"
MODULE_LOADER = "bin/appledrm-insmod"
EXEC_FIELDS = [0, 0o100755, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]
SMOKE_INIT = "aurora-notch-smoke"

SMOKE = b"""#!/bin/sh
set -eu
export PATH=/bin
finish() { /bin/busybox poweroff -f; }
trap finish EXIT
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo AURORA_NOTCH_SMOKE_START
if grep -q '^appledrm ' /proc/modules; then
    echo AURORA_NOTCH_SMOKE_FAIL_ALREADY_LOADED
    exit 1
fi
echo AURORA_NOTCH_SMOKE_FIRST_LOAD
/bin/sh -x /bin/modprobe appledrm
if test -r /sys/module/appledrm/parameters/show_notch; then
    value=$(cat /sys/module/appledrm/parameters/show_notch)
    echo "AURORA_NOTCH_SMOKE_PARAMETER=$value"
    case "$value" in Y|1) ;; *) exit 1;; esac
else
    echo AURORA_NOTCH_SMOKE_NO_SYSFS_PARAMETER
    exit 1
fi
echo AURORA_NOTCH_SMOKE_REPEAT_ALIAS
/bin/sh -x /bin/modprobe of:NdisplayTCapple,t6030-display-subsystem
test "$(grep -c '^appledrm ' /proc/modules)" = 1
echo AURORA_NOTCH_SMOKE_COMPLETE
"""


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--smoke-output", type=Path)
    parser.add_argument("--cc", default="aarch64-linux-musl-gcc")
    args = parser.parse_args()
    outputs = [args.output] + ([args.smoke_output] if args.smoke_output else [])
    if len({p.resolve() for p in outputs}) != len(outputs) or any(p.exists() for p in outputs):
        parser.error("output paths must be distinct new files")
    source = args.source.read_bytes()
    if sha(source) != SOURCE_SHA256:
        parser.error("expected the native battery-ready v3 initrd")

    archive_tools = runpy.run_path(str(Path(__file__).with_name("patch-r1-native-battery-initrd.py")))
    parse, archive = archive_tools["parse"], archive_tools["archive"]
    original = parse(gzip.decompress(source))
    module = original[MODULE][1]
    assert b"parmtype=show_notch:" in module
    fields, loader = original[LOADER]
    guarded = (
        '        if ! /bin/grep -q "^appledrm " /proc/modules; then\n'
        f'            /bin/insmod /{MODULE} || exit 1\n'
        '        fi'
    ).encode()
    assert loader.count(guarded) == 1
    assert loader.count(f"/bin/insmod /{MODULE}".encode()) == 1
    assert MODULE_LOADER not in original
    with tempfile.TemporaryDirectory() as tmp:
        helper = Path(tmp) / "appledrm-insmod"
        subprocess.run([args.cc, "-Os", "-static", "-Wall", "-Wextra", "-Werror",
                        "-Wl,-z,max-page-size=65536", "-s", "-o", str(helper),
                        str(Path(__file__).resolve().parents[1] / "initramfs/appledrm-insmod.c")],
                       check=True)
        helper_bytes = helper.read_bytes()
    assert helper_bytes[:6] == b"\x7fELF\x02\x01" and helper_bytes[18:20] == b"\xb7\x00"
    changed = dict(original)
    call = f"/{MODULE_LOADER} /{MODULE} show_notch=1 ||".encode()
    changed[LOADER] = (fields, loader.replace(guarded, guarded.replace(
        f"/bin/insmod /{MODULE} ||".encode(), call)))
    changed[MODULE_LOADER] = (EXEC_FIELDS, helper_bytes)

    experiment = gzip.compress(archive(changed), compresslevel=6, mtime=0)
    reread = parse(gzip.decompress(experiment))
    assert list(reread) == [*original, MODULE_LOADER]
    assert all(reread[name][1] == entry[1] for name, entry in original.items() if name != LOADER)
    assert all(reread[name][0][1:6] == entry[0][1:6] and reread[name][0][7:11] == entry[0][7:11]
               for name, entry in original.items())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as out:
        out.write(experiment)
    record = dict(source_sha256=sha(source), output_sha256=sha(experiment),
                  changed_entries=[LOADER], added_entries=[MODULE_LOADER],
                  helper_sha256=sha(helper_bytes), appledrm_sha256=sha(module), parameter="show_notch=1")
    if args.smoke_output:
        changed[SMOKE_INIT] = (EXEC_FIELDS, SMOKE)
        smoke = gzip.compress(archive(changed), compresslevel=6, mtime=0)
        args.smoke_output.parent.mkdir(parents=True, exist_ok=True)
        with args.smoke_output.open("xb") as out:
            out.write(smoke)
        record.update(smoke_sha256=sha(smoke), smoke_cmdline=f"rdinit=/{SMOKE_INIT}")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
