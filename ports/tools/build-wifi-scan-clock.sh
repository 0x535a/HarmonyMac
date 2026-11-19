#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?PROJECT}")
work=${2:-/root/harmonyos-work}
generated=$(realpath "${3:?usage: build-wifi-scan-clock.sh PROJECT WORK GENERATED_WLAN_CHIP}")
out=$work/native-wifi-boundary-20261118/build
mkdir -p "$out"
g++ -O2 -Wall -Wextra -Werror -std=c++17 "$project/ports/tests/wifi-scan-clock-test.cpp" -o "$out/clock-unit"
"$out/clock-unit"
printf '1.0 { global: InitWifiVendorHalFuncTable; local: *; };\n' > "$out/clock.map"
"$work/ohos-toolchain/ohos-platform-cxx" -shared -fPIC -O2 -std=c++17 -Wall -Wextra -Werror \
 -fvisibility=hidden -fno-rtti -I"$generated" \
 "$project/ports/hardware/wifi-scan-clock.cpp" -ldl \
 -Wl,--no-undefined,-Bsymbolic-functions,-soname,libwifi_hal_hw.z.so \
 -Wl,--version-script,"$out/clock.map" -o "$out/libwifi_hal_hw.z.so"
sha256sum "$out/libwifi_hal_hw.z.so"
