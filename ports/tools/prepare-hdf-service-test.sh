#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: prepare-hdf-service-test.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
modules=${4:-$work/kernel-test5-package}
test ! -e "$out"
closure=$work/hdf-service-closure2
if ! test -d "$closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$closure" \
        /system/bin/samgr /vendor/bin/hdf_devmgr /vendor/bin/hdf_devhost \
        /system/bin/hilogd /system/bin/hilog /system/bin/param /system/bin/sh \
        /vendor/lib64/liballocator_driver_1.0.z.so /vendor/lib64/liballocator_service_1.0.z.so \
        /system/lib64/libnative_image.so /system/lib64/chipset-sdk/libdisplay_buffer_hdi_impl_v1_3.z.so
fi
bash "$project/ports/tools/prepare-kms-test.sh" "$out" \
    "$work/static-bootstrap-test/root" "$modules" \
    "$work/core-closure2" "$work/aurora-graphics-build/libdisplay_buffer_vdi_impl.z.so" \
    "$work/aurora-graphics-build/buffer-vdi-probe"
root=$out/root
rsync -a "$closure/." "$root/testroot/"
cp "$work/bootstrap-busybox-fullmod/busybox" "$root/bin/busybox"
for app in kill ps; do ln -s busybox "$root/bin/$app"; done
musl-gcc -static -O2 "$project/ports/tests/service-log-tools.c" -o "$root/bin/service-log-tools"
# Debian/glibc strace is confined to /diag in this disposable diagnostic VM.
# It is not installed into the target Harmony root or any final runtime.
mkdir -p "$root/diag"
cp /usr/bin/strace /lib/ld-linux-aarch64.so.1 /lib/aarch64-linux-gnu/libc.so.6 "$root/diag/"
mkdir -p "$root/testroot/vendor/etc/hdfconfig" "$root/testroot/data/samgr" \
    "$root/testroot/data/log/dict" "$root/testroot/data/log/hilog" "$root/testroot/data/service/el0/public/for-all-app/hilog"
for name in passwd group; do
    for tree in system/system vendor; do
        if test -f "/mnt/harmonyos-source/$tree/etc/$name"; then
            mkdir -p "$root/testroot/${tree#system/}/etc"
            cp "/mnt/harmonyos-source/$tree/etc/$name" "$root/testroot/${tree#system/}/etc/"
        fi
    done
done
for tree in system/system vendor; do
    if test -d "/mnt/harmonyos-source/$tree/etc/selinux/targeted/contexts"; then
        mkdir -p "$root/testroot/${tree#system/}/etc/selinux/targeted/contexts"
        cp -a "/mnt/harmonyos-source/$tree/etc/selinux/targeted/contexts/." "$root/testroot/${tree#system/}/etc/selinux/targeted/contexts/"
    fi
done
"$work/hdf-core-source/framework/tools/hc-gen/build/hc-gen" \
    -o "$root/testroot/vendor/etc/hdfconfig/hdf_default.hcb" "$project/ports/config/hdf-allocator-test.hcs"
cp "$work/aurora-graphics-build/libdisplay_buffer_vdi_impl.z.so" "$root/testroot/vendor/lib64/"
"$work/ohos-toolchain/ohos-cc" -O2 "$project/ports/tests/hilog-sockets.c" \
    -o "$root/testroot/system/bin/aurora/hilog-sockets"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 \
    -I"$project/ports/graphics/include" "$project/ports/tests/native-queue-probe.cpp" -ldl \
    -o "$root/testroot/system/bin/aurora/native-queue-probe"
cat > "$root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/binderfs /dev/unix/socket /tmp
mount -t binder binder /dev/binderfs
binder-setup
busybox insmod /bin/virtio_dma_buf.ko
busybox insmod /bin/virtio-gpu.ko
mount --bind /dev /testroot/dev
# A plain /dev bind does not carry nested binderfs mounts into the chroot.
mount --bind /dev/binderfs /testroot/dev/binderfs
mount -t proc proc /testroot/proc
mount --bind /sys /testroot/sys
echo OHOS_HDF_SERVICE_TEST_BEGIN
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -o /tmp/hilogd.trace -e trace=execve,bind,connect,listen,read,recvfrom,recvmsg,fcntl,close,exit_group /bin/busybox chroot /testroot /system/bin/aurora/hilog-sockets > /tmp/hilogd.log 2>&1 &
logd=$!
sleep 1
chroot /testroot /system/bin/hilog -x > /tmp/hilog.log 2>&1 &
logcat=$!
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -o /tmp/samgr.trace -e trace=openat,ioctl,mmap,prctl /bin/busybox chroot /testroot /system/bin/samgr > /tmp/samgr.log 2>&1 &
samgr=$!
sleep 2
chroot /testroot /vendor/bin/hdf_devmgr > /tmp/hdf-developer.log 2>&1 &
devmgr=$!
sleep 2
chroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH=/vendor/lib64:/system/lib64/aurora-mesa; exec /vendor/bin/hdf_devhost -i 0 -n allocator_host -p 0 -s 0' > /tmp/allocator.log 2>&1 &
allocator=$!
sleep 2
chroot /testroot /system/bin/aurora/native-queue-probe > /tmp/native-queue.log 2>&1
echo "OHOS_HDF_QUEUE_RESULT=$?"
service-log-tools stop $allocator $devmgr $samgr $logcat $logd
sleep 1
echo OHOS_HDF_SAMGR_LOG
service-log-tools dump /tmp/samgr.log
echo OHOS_HDF_DEVMGR_LOG
service-log-tools dump /tmp/hdf-developer.log
echo OHOS_HDF_ALLOCATOR_LOG
service-log-tools dump /tmp/allocator.log
echo OHOS_HDF_QUEUE_LOG
service-log-tools dump /tmp/native-queue.log
echo OHOS_HDF_HILOGD_LOG
service-log-tools dump /tmp/hilogd.log
echo OHOS_HDF_HILOG_LOG
service-log-tools dump /tmp/hilog.log
echo OHOS_HDF_SAMGR_TRACE
service-log-tools dump /tmp/samgr.trace
echo OHOS_HDF_HILOGD_TRACE
service-log-tools dump /tmp/hilogd.trace
echo OHOS_HDF_SERVICE_TEST_END
sleep 2
poweroff -f
EOF
chmod 755 "$root/aurora-init"
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared original HDF service fixture %s\n' "$out/initrd.img"
