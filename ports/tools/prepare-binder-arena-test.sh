#!/bin/bash
# Diskless OHOS Binder mapping matrix, including all user-page offsets.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-binder-arena-test.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$work/static-bootstrap-test/root/." "$out/root/"
mkdir -p "$out/root/testroot/dev"
"$work/ohos-toolchain/ohos-cc" -O2 -I"$work/uapi" \
    "$project/ports/tests/binder-arena-probe.c" \
    -o "$out/root/testroot/system/bin/arena-probe"
# Static Debian diagnostic for the native-page control, outside the OHOS root.
gcc -static -O2 -Wl,-z,max-page-size=65536 -I"$work/uapi" \
    "$project/ports/tests/binder-arena-probe.c" -o "$out/root/bin/arena-native"
gcc -nostdlib -static -Wl,-z,max-page-size=65536 -DHMOS_PROCESS_PAGE_SIZE=16384 \
    "$project/ports/tools/hmos-userpages.S" -o "$out/root/bin/native-pages"
cat > "$out/root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/binderfs
mount -t binder binder /dev/binderfs
mount --bind /dev /testroot/dev
mount --bind /dev/binderfs /testroot/dev/binderfs
chroot /testroot /system/bin/arena-probe
echo "BINDER_ARENA_RESULT=$?"
/bin/native-pages /bin/arena-native
echo "BINDER_ARENA_NATIVE_RESULT=$?"
poweroff -f
EOF
chmod 755 "$out/root/aurora-init"
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
