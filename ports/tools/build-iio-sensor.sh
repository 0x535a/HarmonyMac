#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-iio-sensor.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/generic-20261117/sensor/build
reference=$(realpath "${3:?usage: build-iio-sensor.sh PROJECT WORK SENSOR_REFERENCE}")
hdf=$work/hdf-20261115/drivers_hdf_core-OpenHarmony-6.0-Release
libs=$work/lockscreen-20261117/libs
mkdir -p "$out"
includes=(-I"$project/ports/hardware" -I"$reference/generated/sensor" -I"$reference/generated"
    -isystem "$reference/peripheral-6.0/sensor/interfaces/v1_0"
    -isystem "$work/usb-service-20261115/headers/commonlibrary_c_utils-OpenHarmony-6.0-Release/base/include"
    -isystem "$work/usb-service-20261115/headers/communication_ipc-OpenHarmony-6.0-Release/interfaces/innerkits/ipc_core/include"
    -isystem "$work/battery-ipc-20261115/hilog-source/interfaces/native/innerkits/include"
    -isystem "$work/hdf-20261115/third_party/bounds_checking_function/include")
while IFS= read -r dir; do includes+=(-isystem "$dir"); done < <(find "$hdf/interfaces" -type d | sort)
hardware=$project/ports/hardware
g++ -O2 -Wall -Wextra -Werror -std=c++17 -I"$hardware" "$hardware/linux-iio.cpp" \
    "$project/ports/tests/linux-iio-test.cpp" -o "$out/linux-iio-test"
"$out/linux-iio-test" | tee "$out/unit.log"
common=("$work/ohos-toolchain/ohos-platform-cxx" -O2 -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -fno-rtti -D__USER__ "${includes[@]}")
"${common[@]}" "$hardware/linux-iio.cpp" "$hardware/iio-sensors-cli.cpp" -Wl,--no-undefined -o "$out/iio-sensors"
"${common[@]}" -fPIC -shared "$hardware/linux-iio.cpp" "$hardware/iio-sensor-vdi.cpp" \
    -L"$libs" -Wl,-rpath-link,"$libs" -l:libutils.z.so -l:libipc_single.z.so -l:libhdf_utils.z.so \
    -l:libhdf_ipc_adapter.z.so -l:libhdi.z.so -l:libhilog.so -Wl,--no-undefined \
    -Wl,-soname,libhdi_sensor_impl.z.so -o "$out/libhdi_sensor_impl.z.so"
sha256sum "$out/iio-sensors" "$out/libhdi_sensor_impl.z.so"
"${common[@]}" "$hardware/linux-iio.cpp" "$project/ports/tests/iio-vdi-probe.cpp" \
    -L"$libs" -Wl,-rpath-link,"$libs" -l:libutils.z.so -l:libipc_single.z.so \
    -l:libhdf_utils.z.so -l:libhdf_ipc_adapter.z.so -l:libhdf_host.z.so -l:libhdi.z.so -Wl,--no-undefined -o "$out/iio-vdi-probe"
sha256sum "$out/iio-vdi-probe"
"${common[@]}" "$project/ports/tests/sensor-hdi-info.cpp" \
    "$work/generic-20261117/thermal-audit/libsensor_proxy_3.0.z.so" \
    -L"$libs" -Wl,-rpath-link,"$libs" -l:libutils.z.so -l:libipc_single.z.so \
    -l:libhdf_utils.z.so -l:libhdf_ipc_adapter.z.so -l:libhdi.z.so -Wl,--no-undefined -o "$out/sensor-hdi-info"
sha256sum "$out/sensor-hdi-info"
