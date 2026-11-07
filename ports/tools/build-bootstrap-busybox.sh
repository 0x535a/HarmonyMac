#!/bin/bash
# Small static early-boot toolkit. Final PID 1 remains the original OHOS init.
set -euo pipefail
source=${1:?usage: build-bootstrap-busybox.sh BUSYBOX_SOURCE OUTPUT_DIRECTORY}
out=${2:?}
mkdir -p "$out/uapi"
out=$(realpath "$out")
for name in linux asm-generic mtd; do
    test -e "$out/uapi/$name" || ln -s "/usr/include/$name" "$out/uapi/$name"
done
test -e "$out/uapi/asm" || ln -s /usr/include/aarch64-linux-gnu/asm "$out/uapi/asm"
cd "$source"
make allnoconfig > "$out/configure.log" 2>&1
python3 - <<'PY'
from pathlib import Path
options = '''STATIC DESKTOP LONG_OPTS SHOW_USAGE FEATURE_VERBOSE_USAGE
ASH SH_IS_ASH ASH_OPTIMIZE_FOR_SIZE ASH_ALIAS ASH_JOB_CONTROL ASH_BUILTIN_ECHO
ASH_BUILTIN_PRINTF ASH_BUILTIN_TEST ASH_GETOPTS ASH_CMDCMD
CAT CHMOD CHOWN CP CUT DD ECHO ENV FALSE TRUE HEAD LN LS MKDIR MKNOD MV
PRINTF PWD READLINK REALPATH RM RMDIR SLEEP SORT SYNC TAIL TEST TOUCH TR
UNAME WC SHA256SUM GREP SED AWK FIND XARGS
MOUNT FEATURE_MOUNT_FLAGS FEATURE_MOUNT_FSTAB FEATURE_MOUNT_LABEL
UMOUNT FEATURE_UMOUNT_ALL BLKID FEATURE_BLKID_TYPE VOLUMEID FEATURE_VOLUMEID_EXT
MDEV FEATURE_MDEV_CONF INSMOD MODPROBE DEPMOD LSMOD
FEATURE_MODUTILS_ALIAS FEATURE_MODUTILS_SYMBOLS FEATURE_MODPROBE_BLACKLIST
SWITCH_ROOT CHROOT SETSID REBOOT POWEROFF HALT
'''.split()
p = Path('.config')
s = p.read_text()
for option in options:
    s = s.replace(f'# CONFIG_{option} is not set', f'CONFIG_{option}=y')
s = s.replace('CONFIG_DEFAULT_MODULES_DIR=""', 'CONFIG_DEFAULT_MODULES_DIR="/lib/modules"')
s = s.replace('CONFIG_DEFAULT_DEPMOD_FILE=""', 'CONFIG_DEFAULT_DEPMOD_FILE="modules.dep"')
p.write_text(s)
PY
make oldconfig < /dev/null >> "$out/configure.log" 2>&1
make CC=musl-gcc EXTRA_CFLAGS="-idirafter $out/uapi" \
    EXTRA_LDFLAGS='-Wl,-z,max-page-size=65536' -j3 > "$out/build.log" 2>&1
cp busybox "$out/busybox"
cp .config "$out/config"
file "$out/busybox"
readelf -lW "$out/busybox" > "$out/program-headers.txt"
if grep -q INTERP "$out/program-headers.txt"; then
    echo 'Bootstrap must be static' >&2
    exit 1
fi
printf 'Static bootstrap toolkit built: %s\n' "$out/busybox"
