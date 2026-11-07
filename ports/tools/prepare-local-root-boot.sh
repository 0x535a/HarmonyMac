#!/bin/bash
# Tiny initramfs for the local virtual rootfs, distinct from native deployment.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-local-root-boot.sh PROJECT WORK OUTPUT [MODULE_DIRECTORY]}")
work=$(realpath "${2:?}")
out=${3:?}
modules=${4:-$work/render-service-test1/root/bin}
test ! -e "$out"
mkdir -p "$out/root/bin" "$out/root/dev" "$out/root/proc" "$out/root/sys" "$out/root/newroot"
cp "$work/bootstrap-init" "$out/root/init"
cp "$work/bootstrap-busybox-fullmod/busybox" "$out/root/bin/busybox"
cp "$work/binder-setup" "$out/root/bin/binder-setup"
musl-gcc -static -O2 -Wall -Wextra "$project/ports/tools/input-device-permissions.c" \
    -o "$out/root/bin/input-device-permissions"
for name in virtio_dma_buf.ko virtio-gpu.ko hmos_ashmem.ko; do
    cp "$modules/$name" "$out/root/bin/"
done
for name in sh mount mkdir cat grep echo sleep test switch_root; do
    ln -s busybox "$out/root/bin/$name"
done
cat > "$out/root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo LOCAL_ROOT_BOOTSTRAP_BEGIN
mount -t ext4 -o rw,noatime /dev/vda /newroot || exec sh
test "$(cat /newroot/boot/aurora-migration/local-virtual-only)" = 9d776a48-2355-4d23-8890-b506ba38b113 || exec sh
mkdir -p /dev/binderfs /dev/unix/socket /dev/pts /sys/fs/selinux
mount -t binder binder /dev/binderfs
binder-setup
# Match stock OHOS pre-init device access. Privileged token ioctls remain
# restricted by the AccessToken driver; unprivileged processes must read self.
busybox chown 0:3020 /dev/access_token_id
busybox chmod 0666 /dev/access_token_id
mount -t devpts devpts /dev/pts
mount -t selinuxfs selinuxfs /sys/fs/selinux
busybox insmod /bin/hmos_ashmem.ko
busybox insmod /bin/virtio_dma_buf.ko
busybox insmod /bin/virtio-gpu.ko
for node in /dev/dri/card* /dev/dri/renderD*; do
    if test -c "$node"; then busybox chmod 0666 "$node"; fi
done
if test -d /dev/input; then /bin/input-device-permissions 6696 6696; fi
mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/tmp
busybox chmod 1777 /newroot/tmp
echo LOCAL_ROOT_SWITCH_ROOT
mount --move /sys /newroot/sys
mount --move /proc /newroot/proc
mount --move /dev /newroot/dev
exec switch_root /newroot /init --second-stage
EOF
chmod 755 "$out/root/aurora-init"
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img.tmp"
mv "$out/initrd.img.tmp" "$out/initrd.img"
printf 'Prepared local-root bootstrap %s\n' "$out/initrd.img"
