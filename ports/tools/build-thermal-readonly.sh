#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-thermal-readonly.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/generic-20261117/thermal-build
gen=$(realpath "${3:?usage: build-thermal-readonly.sh PROJECT WORK GENERATED_THERMAL}")
hdf=$work/hdf-20261115/drivers_hdf_core-OpenHarmony-6.0-Release
libs=$work/lockscreen-20261117/libs
mkdir -p "$out"
includes=(-I"$gen" -I"$(dirname "$gen")" -I"$project/ports/hardware"
    -isystem "$work/usb-service-20261115/headers/commonlibrary_c_utils-OpenHarmony-6.0-Release/base/include"
    -isystem "$work/usb-service-20261115/headers/communication_ipc-OpenHarmony-6.0-Release/interfaces/innerkits/ipc_core/include"
    -isystem "$work/battery-ipc-20261115/hilog-source/interfaces/native/innerkits/include"
    -isystem "$work/hdf-20261115/third_party/bounds_checking_function/include")
while IFS= read -r dir; do includes+=(-isystem "$dir"); done < <(find "$hdf/interfaces" -type d | sort)
common=("$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra -Wno-unused-parameter -std=c++17 -fno-rtti -D__USER__ "${includes[@]}")
link=(-L"$libs" -Wl,-rpath-link,"$libs" -l:libutils.z.so -l:libipc_single.z.so -l:libhdf_utils.z.so
    -l:libhdf_ipc_adapter.z.so -l:libhdi.z.so -l:libhilog.so -l:libsec_shared.z.so)
server=(); client=()
for name in thermal_types thermal_interface_stub thermal_callback_proxy fan_callback_proxy; do server+=("$gen/v1_1/$name.cpp"); done
for name in thermal_types thermal_interface_proxy thermal_callback_stub fan_callback_stub; do client+=("$gen/v1_1/$name.cpp"); done
# Deliberately exclude generated service templates: their success stubs are
# not a hardware implementation. Only typed IPC transport is reused.
"${common[@]}" -shared -fPIC "$project/ports/hardware/linux-thermal.cpp" \
    "$project/ports/hardware/thermal-readonly-driver.cpp" "${server[@]}" "${link[@]}" \
    -l:libhdf_host.z.so -Wl,--no-undefined,-soname,libaurora_thermal_driver.z.so -o "$out/libaurora_thermal_driver.z.so"
"${common[@]}" "$project/ports/tests/thermal-hdi-probe.cpp" "${client[@]}" "${link[@]}" \
    -Wl,--no-undefined -o "$out/thermal-hdi-probe"
sha256sum "$out/libaurora_thermal_driver.z.so" "$out/thermal-hdi-probe"
