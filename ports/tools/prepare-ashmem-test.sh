#!/bin/bash
set -euo pipefail
out=${1:?usage: prepare-ashmem-test.sh OUTPUT EXISTING_PAGE_TEST_ROOT ASHMEM_MODULE OHOS_PROBE NATIVE_PROBE}
basis=${2:?}
module=${3:?}
ohos=${4:?}
native=${5:?}
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$basis/." "$out/root/"
root=$out/root
cp "$module" "$root/bin/hmos_ashmem.ko"
cp "$native" "$root/bin/ashmem-probe-native"
cp "$ohos" "$root/testroot/system/bin/ashmem-probe"
mkdir -p "$root/testroot/dev"
ln -s busybox "$root/bin/insmod"
cat > "$root/ashmem-test" <<'EOF'
#!/bin/sh
export PATH=/bin
insmod /bin/hmos_ashmem.ko
echo "ASHMEM_MODULE_RESULT=$?"
mount --bind /dev /testroot/dev
/bin/userpages 4k /bin/chroot /testroot /system/bin/ashmem-probe
echo "OHOS_ASHMEM_USER4K_RESULT=$?"
/bin/ashmem-probe-native
echo "ASHMEM_NATIVE16K_RESULT=$?"
EOF
python3 - "$root/init" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
assert s.count('echo HMOS_PAGE_TEST_END') == 1
p.write_text(s.replace('echo HMOS_PAGE_TEST_END', '/bin/sh /ashmem-test\necho HMOS_PAGE_TEST_END'))
PY
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared %s\n' "$out/initrd.img"
