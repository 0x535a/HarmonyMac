#!/bin/bash
set -euo pipefail
out=${1:?usage: prepare-bootstrap-test.sh OUTPUT STATIC_INIT STATIC_BUSYBOX BINDER_SETUP MINIMAL_OHOS_ROOT}
init=${2:?}
busybox=${3:?}
binder=${4:?}
ohos=${5:?}
test ! -e "$out"
mkdir -p "$out/root/bin" "$out/root/dev" "$out/root/proc" "$out/root/sys" "$out/root/testroot"
cp "$init" "$out/root/init"
cp "$busybox" "$out/root/bin/busybox"
cp "$binder" "$out/root/bin/binder-setup"
cp -a "$ohos/." "$out/root/testroot/"
for app in sh mount mkdir cat grep chroot echo sleep poweroff test '['; do
    ln -s busybox "$out/root/bin/$app"
done
cat > "$out/root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo STATIC_BOOTSTRAP_BEGIN
grep -m 1 KernelPageSize /proc/self/smaps
chroot /testroot /system/bin/sh -c 'echo OHOS_SHELL_FROM_STATIC_BOOTSTRAP_OK'
echo "STATIC_BOOTSTRAP_SHELL_RESULT=$?"
if test -x /testroot/system/bin/aurora/ohos-runtime-probe; then
    chroot /testroot /system/bin/aurora/ohos-runtime-probe
    echo "OHOS_NATIVE_RUNTIME_RESULT=$?"
    chroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH=/system/lib64/aurora-mesa; exec /system/bin/aurora/ohos-cxx-probe'
    echo "OHOS_NATIVE_CXX_RESULT=$?"
fi
if test -x /testroot/system/bin/aurora/ark-binary-probe; then
    chroot /testroot /system/bin/aurora/ark-binary-probe /aurora-probe.abc
    echo "OHOS_ORIGINAL_ARK_RESULT=$?"
fi
if test -x /testroot/system/bin/aurora/egl-softpipe-probe; then
    chroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH=/system/lib64/aurora-mesa LIBGL_DRIVERS_PATH=/system/lib64/aurora-mesa/dri GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1; exec /system/bin/aurora/egl-softpipe-probe'
    echo "OHOS_NATIVE_SOFTPIPE_RESULT=$?"
fi
mkdir -p /dev/binderfs
mount -t binder binder /dev/binderfs
binder-setup
echo "STATIC_BOOTSTRAP_BINDER_RESULT=$?"
echo STATIC_BOOTSTRAP_END
sleep 1
poweroff -f
EOF
chmod 755 "$out/root/init" "$out/root/aurora-init"
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared static bootstrap test: %s\n' "$out/initrd.img"
