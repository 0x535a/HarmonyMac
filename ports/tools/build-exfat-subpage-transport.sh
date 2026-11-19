#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?PROJECT}")
work=$(realpath "${2:?WORK}")
out=$work/native-exfat-20261118
mkdir -p "$out"
"$work/ohos-toolchain/ohos-cc" -shared -fPIC -O2 -Wall -Wextra -Werror \
 "$project/ports/hardware/fuse-subpage-transport.c" \
 -Wl,--no-undefined,-soname,libfuse-subpage-transport.so \
 -o "$out/libfuse-subpage-transport.so"
readelf -dW "$out/libfuse-subpage-transport.so" > "$out/transport.dynamic.txt"
readelf -Ws "$out/libfuse-subpage-transport.so" > "$out/transport.symbols.txt"
sha256sum "$out/libfuse-subpage-transport.so"
