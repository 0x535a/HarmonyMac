#!/bin/bash
# Run in the Linux build VM after applying the OHOS identity source delta.
set -euo pipefail
source_tree=${1:?usage: build-test-kernel.sh SOURCE_TREE OUTPUT_RECORD_DIR RELEASE_SUFFIX}
out=${2:?}
release=${3:?}
mkdir -p "$out"
source_tree=$(realpath "$source_tree")
out=$(realpath "$out")
cd "$source_tree"
test -f drivers/accesstokenid/access_tokenid.c
test -f drivers/accesstokenid/Makefile
cp .config "$out/config-before"
scripts/config --set-str LOCALVERSION "$release" \
    --enable ARM64_16K_PAGES --disable ARM64_VA_BITS_52 --enable ARM64_VA_BITS_48 \
    --disable ARM64_PA_BITS_52 --enable ARM64_PA_BITS_48 --disable ARM64_LPA2 \
    --enable ARM64_USER4K_EXPERIMENTAL --enable ACCESS_TOKENID \
    --enable ANDROID_BINDER_IPC --disable ANDROID_BINDER_IPC_RUST \
    --enable ANDROID_BINDERFS --set-str ANDROID_BINDER_DEVICES 'binder,hwbinder,vndbinder' \
    --enable SECURITY_NETWORK --enable SECURITY_PATH --enable SECURITY_SELINUX \
    --set-str LSM 'selinux,landlock,lockdown,yama,loadpin,safesetid,ipe,bpf' \
    --enable HIDRAW --module BT_RFCOMM --enable BT_RFCOMM_TTY --enable VIRTIO_INPUT \
    --module DRM_VIRTIO_GPU --enable DRM_VIRTIO_GPU_KMS

# Keep Apple and generic virtual-platform testing; other SoC families and GPU
# drivers are not part of the J514S target. Verify Apple options after Kconfig.
python3 - <<'PY'
from pathlib import Path
import re, subprocess
for name in re.findall(r'^config (ARCH_[A-Z0-9_]+)$',
                       Path('arch/arm64/Kconfig.platforms').read_text(), re.M):
    if name not in {'ARCH_APPLE', 'ARCH_VEXPRESS'}:
        subprocess.run(['scripts/config', '--disable', name], check=True)
for name in ['DRM_NOUVEAU', 'DRM_AMDGPU', 'DRM_RADEON', 'DRM_I915', 'DRM_XE',
             'DRM_MSM', 'DRM_TEGRA', 'DRM_ROCKCHIP', 'DRM_MESON', 'DRM_PANFROST',
             'DRM_PANTHOR', 'DRM_ETNAVIV', 'DRM_LIMA', 'DRM_VC4', 'DRM_V3D',
             'DRM_MEDIATEK']:
    subprocess.run(['scripts/config', '--disable', name], check=True)
PY
make LLVM=-19 olddefconfig > "$out/olddefconfig.log" 2>&1
python3 - "$out/config-before" <<'PY'
from pathlib import Path
import sys
def config(path):
    return dict(line.split('=', 1) for line in Path(path).read_text().splitlines()
                if line.startswith('CONFIG_') and '=' in line)
before, after = config(sys.argv[1]), config('.config')
lost = {key: [value, after.get(key)] for key, value in before.items()
        if ('APPLE' in key or 'ASAHI' in key or 'T6030' in key)
        and value in {'y', 'm'} and after.get(key) != value}
if lost:
    raise SystemExit(f'Apple configuration regression: {lost}')
for key in ['CONFIG_ARM64_16K_PAGES', 'CONFIG_ARM64_USER4K_EXPERIMENTAL',
            'CONFIG_ACCESS_TOKENID', 'CONFIG_ANDROID_BINDER_IPC']:
    assert after.get(key) == 'y', (key, after.get(key))
print('Required page/IPC configuration and existing Apple options preserved.')
PY
cp .config "$out/config"
make LLVM=-19 -j3 Image modules dtbs > "$out/build.log" 2>&1
make LLVM=-19 kernelrelease > "$out/kernelrelease"
cp arch/arm64/boot/Image "$out/Image"
printf 'Build completed: %s\n' "$out"
