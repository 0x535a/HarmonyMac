#!/bin/bash
# Disposable initramfs: Debian diagnostics outside a minimal original OHOS root.
# This is a compatibility test, not the final product rootfs.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
out=${1:?usage: prepare-page-test.sh OUTPUT_DIRECTORY SOURCE_ROOT PAGE_PROBE_C}
source_root=${2:?}
probe_source=${3:?}
kernel_source=${4:-}
ipc_probe=${5:-}
freestanding_launcher=${6:-}
test ! -e "$out"
mkdir -p "$out/root" "$out/hmos/system/bin" "$out/hmos/system/lib" \
    "$out/hmos/system/lib64" "$out/hmos/system/etc"
cp "$source_root/system/system/bin/sh" "$out/hmos/system/bin/sh"
cp "$source_root/system/system/lib/ld-musl-aarch64.so.1" "$out/hmos/system/lib/"
cp "$source_root/system/system/etc/ld-musl-aarch64.path" "$out/hmos/system/etc/"
ln -s ../lib/ld-musl-aarch64.so.1 "$out/hmos/system/lib64/libc.so"
ln -s system/lib "$out/hmos/lib"
ln -s system/etc "$out/hmos/etc"
chmod 755 "$out/hmos/system/bin/sh" "$out/hmos/system/lib/ld-musl-aarch64.so.1"
gcc -O2 -Wall -Wextra -Werror "$probe_source" -o "$out/page-probe"
root=$out/root
mkdir -p "$root/bin" "$root/proc" "$root/sys" "$root/dev" "$root/testroot"
cp /bin/busybox "$root/bin/busybox"
cp "$out/page-probe" "$root/bin/page-probe"
if test -n "$ipc_probe"; then cp "$ipc_probe" "$root/bin/ipc-probe"; fi
if test -n "$freestanding_launcher"; then
    cp "$freestanding_launcher" "$root/bin/hmos-userpages"
fi
if test -n "$kernel_source"; then
    make -C "$kernel_source/tools/aurora-userpages"
    cp "$kernel_source/tools/aurora-userpages/userpages" "$root/bin/userpages"
    gcc -O2 -ffixed-x18 -Wl,-z,max-page-size=65536 \
        "$kernel_source/tools/testing/selftests/arm64/user4k/page-contract.c" \
        "$kernel_source/tools/testing/selftests/arm64/user4k/x18-probe.S" \
        -o "$root/bin/page-contract"
fi
for executable in /bin/busybox "$out/page-probe"; do
    while IFS= read -r lib; do
        mkdir -p "$root$(dirname "$lib")"
        cp -L "$lib" "$root$lib"
    done < <(ldd "$executable" | awk '/=> \// {print $3} /^\s*\/|^[[:space:]]*\// {print $1}')
done
for app in sh mount mkdir chroot echo cat sleep poweroff; do
    ln -s busybox "$root/bin/$app"
done
cp -a "$out/hmos/." "$root/testroot/"
cat > "$root/init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo HMOS_PAGE_TEST_BEGIN
cat /proc/cmdline
page-probe /bin/chroot /testroot /system/bin/sh -c 'echo HMOS_ORIGINAL_SHELL_OK'
echo "HMOS_PAGE_TEST_RESULT=$?"
if test -x /bin/userpages; then
    /bin/userpages --status
    /bin/userpages 4k /bin/page-probe /bin/chroot /testroot /system/bin/sh -c 'echo HMOS_ORIGINAL_SHELL_USER4K_OK'
    echo "HMOS_USER4K_SHELL_RESULT=$?"
    /bin/userpages 4k /bin/page-contract --expect-4k
    echo "HMOS_USER4K_CONTRACT_RESULT=$?"
fi
if test -x /bin/hmos-userpages; then
    /bin/hmos-userpages /bin/chroot /testroot /system/bin/sh -c 'echo HMOS_FREESTANDING_LAUNCHER_OK'
    echo "HMOS_FREESTANDING_RESULT=$?"
fi
if test -x /bin/ipc-probe; then
    mkdir -p /dev/binderfs
    mount -t binder binder /dev/binderfs
    /bin/userpages 4k /bin/ipc-probe
    echo "HMOS_IPC_USER4K_RESULT=$?"
    /bin/ipc-probe
    echo "HMOS_IPC_NATIVE_RESULT=$?"
fi
echo HMOS_PAGE_TEST_END
sleep 1
poweroff -f
sleep 5
EOF
chmod 755 "$root/init"
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared %s\n' "$out"
