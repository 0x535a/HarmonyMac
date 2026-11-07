#!/bin/bash
set -euo pipefail
project=${1:?usage: build-buffer-vdi.sh PROJECT_SOURCE WORK_DIRECTORY}
work=${2:?}
project=$(realpath "$project")
work=$(realpath "$work")
out=$work/aurora-graphics-build
mkdir -p "$out"
base=$project/ports/graphics
prefix=$work/ohos-deps/system
platform=/mnt/harmonyos-source/system/system/lib64/chipset-pub-sdk/libbuffer_handle.z.so
test -f "$platform"
"$work/ohos-toolchain/ohos-cc" -fPIC -O2 -Wall -Wextra -Werror \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    -c "$base/drm_device.c" -o "$out/drm_device.o"
"$work/ohos-toolchain/ohos-platform-cxx" -shared -fPIC -O2 -std=c++17 -Wall -Wextra \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    "$base/buffer_vdi.cpp" "$out/drm_device.o" \
    -L"$prefix/lib64/aurora-mesa" -ldrm "$platform" \
    -Wl,--no-undefined,-soname,libdisplay_buffer_vdi_impl.z.so \
    -Wl,-rpath,/system/lib64/aurora-mesa \
    -o "$out/libdisplay_buffer_vdi_impl.z.so" > "$out/buffer-vdi-build.log" 2>&1
readelf -dW "$out/libdisplay_buffer_vdi_impl.z.so" > "$out/buffer-vdi-dynamic.txt"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    "$project/ports/tests/buffer-vdi-probe.cpp" "$out/drm_device.o" \
    -L"$prefix/lib64/aurora-mesa" -lEGL -lGLESv2 -lgbm -ldrm -ldl \
    -Wl,-rpath,/system/lib64/aurora-mesa \
    -o "$out/buffer-vdi-probe" > "$out/buffer-vdi-probe-build.log" 2>&1
printf 'Built %s\n' "$out/libdisplay_buffer_vdi_impl.z.so"
