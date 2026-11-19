#!/bin/bash
# Build OHOS artifacts and Linux unit probes in an explicit work directory.
set -euo pipefail
project=$(realpath "${1:?usage: build-hardware.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/hardware-20261115
mkdir -p "$out"
cc=$work/ohos-toolchain/ohos-cc
hardware=$project/ports/hardware
"$cc" -O2 -fPIC -Wall -Wextra -Werror -shared \
    "$hardware/xpower_battery.c" "$hardware/power_supply.c" -pthread \
    -Wl,--no-undefined,-soname,libxpower_battery_v303.so \
    -o "$out/libxpower_battery_v303.so"
"$cc" -O2 -Wall -Wextra -Werror "$project/ports/tests/time-rtc-probe.c" \
    -ldl -o "$out/time-rtc-probe"
"$cc" -std=c11 -O2 -Wall -Wextra -Werror "$hardware/storage-mount-root.c" \
    -o "$out/storage-mount-root"
for probe in backlight power-supply; do
    source=$project/ports/tests/$probe-test.c
    gcc -O2 -Wall -Wextra -Werror "$source" -pthread -o "$out/$probe-test-linux"
    "$out/$probe-test-linux" | tee "$out/$probe-linux.log"
    "$cc" -O2 -Wall -Wextra -Werror "$source" -pthread -o "$out/$probe-test-ohos"
done
readelf -dW "$out/libxpower_battery_v303.so" > "$out/xpower-dynamic.txt"
readelf -Ws "$out/libxpower_battery_v303.so" > "$out/xpower-symbols.txt"
