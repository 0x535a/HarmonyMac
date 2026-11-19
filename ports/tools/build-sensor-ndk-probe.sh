#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-sensor-ndk-probe.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/generic-20261117/sensor/build
"$work/ohos-toolchain/ohos-cc" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -I"$work/sdk/native-extracted/native/sysroot/usr/include/sensors" \
    "$project/ports/tests/sensor-ndk-probe.c" -ldl -Wl,--no-undefined -o "$out/sensor-ndk-probe"
sha256sum "$out/sensor-ndk-probe"
