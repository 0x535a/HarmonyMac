#!/bin/bash
# Build the complete official portable backend; no credential-query stubs.
set -euo pipefail
project=$(realpath "${1:?usage: build-userauth-reference.sh PROJECT WORK ROOT}")
work=$(realpath "${2:?}")
root=$(realpath "${3:?}")
out=$work/lockscreen-20261117
src=$out/patched-userauth
hdf=$work/hdf-20261115/drivers_hdf_core-OpenHarmony-6.0-Release
gen=$out/generated
mkdir -p "$out/libs" "$out/objects"
python3 - "$out/reference/user_auth/hdi_service" "$src" <<'PY'
import shutil,sys
shutil.copytree(sys.argv[1], sys.argv[2], dirs_exist_ok=True)
PY
patch --batch -s -p1 -d "$src" < "$project/ports/hardware/patches/userauth-state-integrity.patch"
patch --batch -s -p1 -d "$src" < "$project/ports/hardware/patches/userauth-hdf-registration.patch"
for file in libutils.z.so libipc_single.z.so libhdf_utils.z.so libhdf_ipc_adapter.z.so libhdi.z.so libhilog.so libsec_shared.z.so; do
    cp "$root/system/lib64/chipset-sdk-sp/$file" "$out/libs/$file"
done
cp "$root/vendor/lib64/libhdf_host.z.so" "$out/libs/"
cp "$root/system/lib64/chipset-sdk-sp/libcrypto_openssl.z.so" "$out/libs/"
for file in UserAuthTypes IMessageCallback IUserAuthInterface; do
    "$hdf/framework/tools/hdi-gen/hdi-gen" -s full -m ipc -l cpp \
        -r "ohos.hdi:$out/reference/interfaces" \
        -c "$out/reference/interfaces/user_auth/v4_0/$file.idl" -d "$gen"
done
includes=(-I"$out/crypto-headers" -I"$gen" -I"$gen/user_auth" -I"$gen/user_auth/v4_0"
    -I"$src/interface" -I"$src/hal_sdk" -I"$out/reference/user_auth/common/logs" -I"$out/reference/user_auth/common/utils"
    -I"$work/battery-ipc-20261115/hilog-source/interfaces/native/innerkits/include"
    -I"$work/hdf-20261115/third_party/bounds_checking_function/include"
    -I"$work/usb-service-20261115/headers/commonlibrary_c_utils-OpenHarmony-6.0-Release/base/include"
    -I"$work/usb-service-20261115/headers/communication_ipc-OpenHarmony-6.0-Release/interfaces/innerkits/ipc_core/include")
while IFS= read -r dir; do includes+=(-I"$dir"); done < <(find "$hdf/interfaces" "$src" -type d | sort)
common=(-fPIC -O2 -Wall -Wextra -D__USER__)
objects=()
while IFS= read -r file; do
    [[ "$file" == */file_operator.c ]] && continue
    obj=$out/objects/$(basename "${file%.c}").o
    "$work/ohos-toolchain/ohos-cc" "${common[@]}" "${includes[@]}" -c "$file" -o "$obj"
    objects+=("$obj")
done < <(find "$src" -name '*.c' | sort)
base_objects=("${objects[@]}")
"$work/ohos-toolchain/ohos-cc" "${common[@]}" -Werror "${includes[@]}" \
    -c "$project/ports/hardware/userauth-file.c" -o "$out/objects/aurora_file.o"
objects+=("$out/objects/aurora_file.o")
cxx=("$work/ohos-toolchain/ohos-platform-cxx" "${common[@]}" -std=c++17 -fno-rtti -Wno-c++11-narrowing "${includes[@]}")
libs=(-L"$out/libs" -Wl,-rpath-link,"$out/libs" -l:libutils.z.so -l:libipc_single.z.so
    -l:libhdf_utils.z.so -l:libhdf_ipc_adapter.z.so -l:libhdi.z.so -l:libhilog.so -l:libsec_shared.z.so)
mapfile -t protocol < <(find "$gen/user_auth/v4_0" -name '*.cpp' ! -name '*service.cpp' ! -name '*driver.cpp' ! -name 'user_auth_interface_proxy.cpp' | sort)
"${cxx[@]}" -shared "${protocol[@]}" "${libs[@]}" \
    -Wl,--no-undefined,-soname,libuser_auth_stub_4.0.z.so -o "$out/libuser_auth_stub_4.0.z.so"
"${cxx[@]}" -shared "$src/service/user_auth_interface_service.cpp" "$src/hal_sdk/useriam_common.cpp" \
    "${objects[@]}" "$out/libuser_auth_stub_4.0.z.so" "${libs[@]}" -l:libcrypto_openssl.z.so \
    -Wl,--no-undefined,-soname,libuser_auth_interface_service_4.0.z.so -o "$out/libuser_auth_interface_service_4.0.z.so"
"${cxx[@]}" -shared "$src/service/user_auth_interface_driver.cpp" "$out/libuser_auth_interface_service_4.0.z.so" \
    "$out/libuser_auth_stub_4.0.z.so" "${libs[@]}" -l:libhdf_host.z.so \
    -Wl,--no-undefined,-soname,libuser_auth_driver.z.so -o "$out/libuser_auth_driver.z.so"
"$work/ohos-toolchain/ohos-cc" "${common[@]}" -Werror "${includes[@]}" \
    '-DAURORA_USERAUTH_STORAGE_BASE="/data/boot/aurora/userauth-fixture-data/core"' \
    -c "$project/ports/hardware/userauth-file.c" -o "$out/objects/fixture_file.o"
"${cxx[@]}" -shared "$src/service/user_auth_interface_service.cpp" "$src/hal_sdk/useriam_common.cpp" \
    "${base_objects[@]}" "$out/objects/fixture_file.o" "$out/libuser_auth_stub_4.0.z.so" "${libs[@]}" -l:libcrypto_openssl.z.so \
    -Wl,--no-undefined,-soname,libaurora_userauth_fixture.so -o "$out/libaurora_userauth_fixture.so"
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror "${includes[@]}" \
    '-DAURORA_USERAUTH_STORAGE_BASE="/data/boot/aurora/userauth-fixture-data/io"' \
    "$project/ports/hardware/userauth-file.c" "$project/ports/tests/userauth-file-test.c" \
    -Wl,--no-undefined -o "$out/userauth-file-test"
"${cxx[@]}" "$project/ports/tests/userauth-core-probe.cpp" "${libs[@]}" -ldl \
    -Wl,--no-undefined -o "$out/userauth-core-probe"
for file in libuser_auth_driver.z.so libuser_auth_stub_4.0.z.so libuser_auth_interface_service_4.0.z.so; do
    readelf -dW "$out/$file" > "$out/$file.dynamic.txt"
    sha256sum "$out/$file"
done
