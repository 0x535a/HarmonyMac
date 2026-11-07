#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-egl-window-bridge.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/aurora-graphics-build
prefix=$work/ohos-deps/system
base=$project/ports/graphics
mkdir -p "$out"
"$work/ohos-toolchain/ohos-cc" -fPIC -O2 -Wall -Wextra -Werror \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    -c "$base/drm_device.c" -o "$out/drm_device.o"
"$work/ohos-toolchain/ohos-platform-cxx" -shared -fPIC -O2 -std=c++17 -Wall -Wextra \
    -DEGL_EGLEXT_PROTOTYPES -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    "$base/egl_window_bridge.cpp" "$out/drm_device.o" \
    -L"$prefix/lib64/aurora-mesa" -lGLESv2 -lgbm -ldrm -ldl \
    -Wl,--no-undefined,-soname,libEGL_impl.so -Wl,-Bsymbolic-functions \
    -Wl,-rpath,/system/lib64/aurora-mesa \
    -o "$out/libEGL_impl.so" > "$out/egl-window-bridge-build.log" 2>&1
readelf -dW "$out/libEGL_impl.so" > "$out/egl-window-bridge-dynamic.txt"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra -DEGL_EGLEXT_PROTOTYPES \
    -I"$base/include" -I"$prefix/include" \
    "$project/ports/tests/egl-window-probe.cpp" \
    -L"$out" -lEGL_impl -L"$prefix/lib64/aurora-mesa" -lGLESv2 -ldl \
    -Wl,--export-dynamic -Wl,-rpath,/vendor/lib64/aurora-graphics:/system/lib64/aurora-mesa \
    -o "$out/egl-window-probe" > "$out/egl-window-probe-build.log" 2>&1
printf 'Built %s\n' "$out/libEGL_impl.so"
