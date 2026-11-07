#!/bin/bash
# Disposable ABI diagnostics; Debian tools here are outside the target runtime.
set -euo pipefail
out=${1:?usage: prepare-bpf-ringbuf-test.sh OUTPUT PAGE_TEST_ROOT NATIVE_PROBE OHOS_PROBE}
basis=${2:?}
native=${3:?}
ohos=${4:?}
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$basis/." "$out/root/"
root=$out/root
cp "$native" "$root/bin/bpf-ringbuf-probe"
cp "$ohos" "$root/testroot/system/bin/bpf-ringbuf-probe"
cat > "$root/init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo BPF_RING_TEST_BEGIN
cat /proc/cmdline
/bin/bpf-ringbuf-probe
echo "BPF_RING_NATIVE_RESULT=$?"
/bin/userpages 4k /bin/bpf-ringbuf-probe
echo "BPF_RING_GLIBC4K_RESULT=$?"
/bin/userpages 4k /bin/chroot /testroot /system/bin/bpf-ringbuf-probe
echo "BPF_RING_OHOS4K_RESULT=$?"
echo BPF_RING_TEST_END
poweroff -f
EOF
chmod 755 "$root/init"
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared %s\n' "$out"
