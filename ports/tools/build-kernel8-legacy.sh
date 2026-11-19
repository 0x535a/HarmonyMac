#!/bin/bash
# Independent test8 tuple; preserve all test7 source/build products.
set -euo pipefail
source_tree=$(realpath "${1:?usage: build-kernel8-legacy.sh SOURCE PROJECT OUTPUT [REUSE_STOPPED_TREE]}")
project=$(realpath "${2:?}")
output=${3:?}
test ! -e "$output"
mkdir -p "$output"
output=$(realpath "$output")
test "$(sha256sum "$source_tree/.config" | cut -d' ' -f1)" = a8dd37aab943619b442a0b02f8d1189c3140c691d6cf1ebe7557d69bb89ebcd0
if test -n "${4:-}"; then
    reuse=$(realpath "$4")
    test "$reuse" != "$source_tree"
    test "$(sha256sum "$reuse/.config" | cut -d' ' -f1)" = "$(sha256sum "$(dirname "$reuse")/config" | cut -d' ' -f1)"
    printf 'Reusing explicitly stopped independent build tree: %s\n' "$reuse"
    ln -s "$reuse" "$output/source"
else
    printf 'Copying retained test7 tree into independent build\n'
    cp -a "$source_tree" "$output/source"
fi
cd "$output/source"
cp .config "$output/config-input"
cp "$source_tree/.config" "$output/config-before"
scripts/config --set-str LOCALVERSION '-aurora-hmos-j514s-test8' --enable NETFILTER_XTABLES_LEGACY --enable HOTPLUG_PCI_PCIE
for option in IP_NF_IPTABLES_LEGACY IP6_NF_IPTABLES_LEGACY IP_NF_FILTER IP_NF_MANGLE \
    IP_NF_RAW IP_NF_NAT IP6_NF_FILTER IP6_NF_MANGLE IP6_NF_RAW IP6_NF_NAT; do
    scripts/config --module "$option"
done
make LLVM=-19 olddefconfig > "$output/olddefconfig.log" 2>&1
python3 - "$output/config-before" "$output" <<'PY'
import json,re,sys
from pathlib import Path
def config(p): return dict(re.findall(r'^CONFIG_(\w+)=(.*)$',Path(p).read_text(),re.M))
before,after=config(sys.argv[1]),config('.config')
lost={k:[v,after.get(k)] for k,v in before.items() if v in ['y','m'] and after.get(k)!=v}
assert not lost,('existing enabled option lost',lost)
for key in ['ARM64_16K_PAGES','ARM64_USER4K_EXPERIMENTAL','ACCESS_TOKENID','ANDROID_BINDER_IPC','PREEMPT']:
 assert before.get(key)==after.get(key)=='y',key
assert after.get('NETFILTER_XTABLES_LEGACY')=='y'
assert after.get('HOTPLUG_PCI_PCIE')=='y'
assert after.get('IP_NF_IPTABLES_LEGACY')==after.get('IP6_NF_IPTABLES_LEGACY')=='m'
out=Path(sys.argv[2]);(out/'config-delta.json').write_text(json.dumps({k:[before.get(k,'n'),v] for k,v in after.items() if before.get(k)!=v},indent=2)+'\n')
PY
cp .config "$output/config"
printf 'Building independent Image, modules and DTBs\n'
make LLVM=-19 -j3 Image modules dtbs > "$output/build.log" 2>&1
release=$(make -s LLVM=-19 kernelrelease)
test "$release" = 7.2.11-aurora-hmos-j514s-test8
printf '%s\n' "$release" > "$output/kernelrelease"
cp arch/arm64/boot/Image Module.symvers System.map "$output/"
make LLVM=-19 INSTALL_MOD_PATH="$output/package" INSTALL_MOD_STRIP=1 modules_install > "$output/modules-install.log" 2>&1
mkdir "$output/ashmem"
cp "$project/ports/kernel/ashmem/"* "$output/ashmem/"
make LLVM=-19 M="$output/ashmem" -j3 modules > "$output/ashmem-build.log" 2>&1
cp "$output/ashmem/hmos_ashmem.ko" "$output/package/lib/modules/$release/"
depmod -b "$output/package" "$release"
python3 - "$output" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);release=(r/'kernelrelease').read_text().strip()
for name in ['ip_tables.ko','iptable_filter.ko','ip6_tables.ko','ip6table_filter.ko','virtio-gpu.ko','hmos_ashmem.ko']:
 assert list((r/'package').rglob(name)),name
paths=['Image','config','Module.symvers','System.map','package/lib/modules/'+release+'/hmos_ashmem.ko']
(r/'manifest.json').write_text(json.dumps(dict(release=release,source_baseline='d13466ebb8b5f2e2af3d2c8359f4c96e711a5679',
 files={p:hashlib.sha256((r/p).read_bytes()).hexdigest() for p in paths},runtime_verified=False),indent=2)+'\n')
PY
printf 'KERNEL8_BUILD_COMPLETE release=%s\n' "$release"
