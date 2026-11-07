#!/bin/bash
# Real ISO ConsumerSurface/NativeWindow + original SAMgr/HDF + OHOS EGL bridge.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-real-queue-egl-test.sh PROJECT WORK OUTPUT [MODULES]}")
work=$(realpath "${2:?}")
out=${3:?}
modules=${4:-$work/kernel-test6-package}
bash "$project/ports/tools/prepare-init-selinux-test.sh" "$project" "$work" "$out" "$modules"
graphics=$work/aurora-graphics-build
prefix=$work/ohos-deps/system
bash "$project/ports/tools/build-egl-window-bridge.sh" "$project" "$work"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra \
    -DNATIVE_QUEUE_EGL -DEGL_EGLEXT_PROTOTYPES \
    -I"$project/ports/graphics/include" -I"$prefix/include" \
    "$project/ports/tests/native-queue-probe.cpp" \
    -L"$graphics" -lEGL_impl -L"$prefix/lib64/aurora-mesa" -lGLESv2 -ldl \
    -Wl,-rpath,/vendor/lib64/aurora-graphics:/system/lib64/aurora-mesa \
    -o "$out/root/testroot/system/bin/aurora/native-queue-egl-probe"
cp "$graphics/libEGL_impl.so" "$out/root/testroot/vendor/lib64/aurora-graphics/"
# Bind NativeImage's actual DT_NEEDED edge to the adapter. Merely linking the
# test executable to it does not put extensions in a later dlopen's scope.
ln -sfn /vendor/lib64/aurora-graphics/libEGL_impl.so "$out/root/testroot/system/lib64/libEGL.so"
ln -sfn libEGL_impl.so "$out/root/testroot/vendor/lib64/aurora-graphics/libEGL.so"
python3 - "$out/root/testroot/system/bin/aurora/service-test.sh" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
s = s.replace('export PATH=/system/bin/aurora:/system/bin\n',
              'export PATH=/system/bin/aurora:/system/bin\n'
              'export GBM_BACKENDS_PATH=/system/lib64/aurora-mesa/gbm GALLIUM_DRIVER=softpipe '
              'LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=kms_swrast\n')
assert s.count('/system/bin/aurora/native-queue-probe') == 1
s = s.replace('/system/bin/aurora/native-queue-probe', '/system/bin/aurora/native-queue-egl-probe')
p.write_text(s)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img.tmp"
mv "$out/initrd.img.tmp" "$out/initrd.img"
printf 'Prepared real NativeWindow EGL fixture %s\n' "$out/initrd.img"
