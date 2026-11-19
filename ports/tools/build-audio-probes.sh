#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-audio-probes.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/audio-service-20261115
mkdir -p "$out/include"
if ! test -d "$out/include/alsa"; then cp -a /usr/include/alsa "$out/include/alsa"; fi
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror -I"$out/include" \
    "$project/ports/tests/alsa-pcm-probe.c" "$out/libs/libasound.so" \
    -Wl,--allow-shlib-undefined -Wl,-rpath,/vendor/lib64 -o "$out/alsa-pcm-probe" > "$out/probe-build.log" 2>&1
readelf -dW "$out/alsa-pcm-probe" > "$out/probe-dynamic.txt"
sha256sum "$out/alsa-pcm-probe"
