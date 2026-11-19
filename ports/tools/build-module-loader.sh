#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-module-loader.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/kmod-ohos-20261115
cc=$work/ohos-toolchain/ohos-cc
gcc -O2 -Wall -Wextra -Werror "$project/ports/tests/module-uevent-test.c" \
    "$project/ports/hardware/module-uevent.c" -o "$out/module-uevent-linux"
"$out/module-uevent-linux" | tee "$out/module-uevent-linux.log"
"$cc" -O2 -Wall -Wextra -Werror "$project/ports/tests/module-uevent-test.c" \
    "$project/ports/hardware/module-uevent.c" -o "$out/module-uevent-ohos"
"$cc" -O2 -Wall -Wextra -Werror -I"$out/kmod-34.2" \
    "$project/ports/hardware/module-loader.c" "$project/ports/hardware/module-uevent.c" \
    "$out/build/libkmod-internal.a" "$out/build/libshared.a" \
    -o "$out/module-loader" > "$out/module-loader-build.log" 2>&1
readelf -dW "$out/module-loader" > "$out/module-loader-dynamic.txt"
sha256sum "$out/module-loader"
