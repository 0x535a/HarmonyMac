#!/bin/bash
set -euo pipefail
out=${1:?usage: prepare-kms-test.sh OUTPUT BASIS_ROOT MODULE_PACKAGE OHOS_CLOSURE VDI_LIBRARY PROBE}
basis=${2:?}
modules=${3:?}
closure=${4:?}
vdi=${5:?}
probe=${6:?}
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$basis/." "$out/root/"
root=$out/root
ln -sf busybox "$root/bin/test"
ln -sf busybox "$root/bin/["
mkdir -p "$root/testroot/vendor/lib64/aurora-graphics" "$root/testroot/dev" "$root/testroot/proc" "$root/testroot/sys"
rsync -a "$closure/." "$root/testroot/"
cp "$vdi" "$root/testroot/vendor/lib64/aurora-graphics/"
cp "$probe" "$root/testroot/system/bin/aurora/buffer-vdi-probe"
for name in virtio_dma_buf.ko virtio-gpu.ko; do
    file=$(find "$modules/lib/modules" -name "$name" -type f)
    test -n "$file"
    cp "$file" "$root/bin/$name"
    llvm-strip-19 --strip-debug "$root/bin/$name"
done
cat > "$root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo OHOS_KMS_TEST_BEGIN
/bin/busybox insmod /bin/virtio_dma_buf.ko
/bin/busybox insmod /bin/virtio-gpu.ko
sleep 1
/bin/busybox ls -l /dev/dri
mount --bind /dev /testroot/dev
mount -t proc proc /testroot/proc
mount --bind /sys /testroot/sys
chroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH=/system/lib64/aurora-mesa GBM_BACKENDS_PATH=/system/lib64/aurora-mesa/gbm GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=kms_swrast EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose; exec /system/bin/aurora/buffer-vdi-probe'
echo "OHOS_KMS_TEST_RESULT=$?"
echo OHOS_KMS_TEST_END
sleep 3
poweroff -f
EOF
chmod 755 "$root/aurora-init"
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared %s\n' "$out/initrd.img"
