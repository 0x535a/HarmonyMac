#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: prepare-native-queue-test.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$work/static-bootstrap-test/root/." "$out/root/"
if ! test -d "$work/native-image-closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source \
        "$work/native-image-closure" /system/lib64/libnative_image.so /system/bin/sh
fi
mkdir -p "$out/root/testroot"
rsync -a "$work/core-closure2/." "$out/root/testroot/"
rsync -a "$work/native-image-closure/." "$out/root/testroot/"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 \
    -I"$project/ports/graphics/include" "$project/ports/tests/native-queue-probe.cpp" -ldl \
    -o "$out/root/testroot/system/bin/aurora/native-queue-probe"
cat > "$out/root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount --bind /dev /testroot/dev
mount -t proc proc /testroot/proc
mount --bind /sys /testroot/sys
echo OHOS_NATIVE_QUEUE_TEST_BEGIN
chroot /testroot /system/bin/aurora/native-queue-probe
echo "OHOS_NATIVE_QUEUE_TEST_RESULT=$?"
echo OHOS_NATIVE_QUEUE_TEST_END
sleep 2
poweroff -f
EOF
chmod 755 "$out/root/aurora-init"
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared original NativeWindow queue fixture %s\n' "$out/initrd.img"
