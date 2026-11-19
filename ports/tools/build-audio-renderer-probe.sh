#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?PROJECT}")
work=$(realpath "${2:?WORK}")
out=$work/audio-service-20261115
mkdir -p "$out"
"$work/ohos-toolchain/ohos-cc" -O2 -std=gnu11 -Wall -Wextra \
    "$project/ports/tests/audio-renderer-probe.c" -ldl \
    -o "$out/audio-renderer-probe" > "$out/audio-renderer-build.log" 2>&1
readelf -hW "$out/audio-renderer-probe"
