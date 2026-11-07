#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-composer.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/aurora-graphics-build
prefix=$work/ohos-deps/system
base=$project/ports/graphics
mkdir -p "$out"
"$work/ohos-toolchain/ohos-cc" -fPIC -O2 -Wall -Wextra -Werror \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    -c "$base/drm_device.c" -o "$out/drm_device.o"
"$work/ohos-toolchain/ohos-platform-cxx" -shared -fPIC -O2 -std=c++17 -Wall -Wextra \
    -I"$base/include" -I"$prefix/include" -I"$prefix/include/libdrm" \
    "$base/composer.cpp" "$out/drm_device.o" -L"$prefix/lib64/aurora-mesa" -ldrm \
    -Wl,--no-undefined,-soname,libdisplay_composer_vdi_impl.z.so \
    -Wl,-rpath,/system/lib64/aurora-mesa \
    -o "$out/libdisplay_composer_vdi_impl.z.so" > "$out/composer-build.log" 2>&1
readelf -dW "$out/libdisplay_composer_vdi_impl.z.so" > "$out/composer-dynamic.txt"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra \
    -I"$base/include" "$project/ports/tests/composer-probe.cpp" -ldl \
    -o "$out/composer-probe" > "$out/composer-probe-build.log" 2>&1
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra \
    -I"$base/include" "$project/ports/tests/composer-service-probe.cpp" -ldl \
    -o "$out/composer-service-probe" > "$out/composer-service-probe-build.log" 2>&1
printf 'Built %s\n' "$out/libdisplay_composer_vdi_impl.z.so"
