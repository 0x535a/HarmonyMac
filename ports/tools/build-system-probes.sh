#!/bin/bash
# OHOS-ABI diagnostics for full-root service and ArkUI integration.
set -euo pipefail
project=$(realpath "${1:?usage: build-system-probes.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
mkdir -p "$out"
for name in render-service-probe render-scene-probe token-permission-probe arkui-global-probe hilog-decode-message i18n-language-probe; do
    "$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra \
        "$project/ports/tests/$name.cpp" -ldl -o "$out/$name"
done
for name in token-dump input-device-probe ark-binary-probe native-vsync-probe hdi-service-ready device-identity-probe; do
    "$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra \
        "$project/ports/tests/$name.c" -ldl -o "$out/$name"
done
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra \
    -I"$project/ports/tests/include" \
    "$project/ports/tests/app-stack-probe.cpp" \
    /mnt/harmonyos-source/system/system/lib64/platformsdk/libdfx_dumpcatcher.z.so \
    -o "$out/app-stack-probe"
headers=$work/samgr-probe-headers
for name in samgr-service-probe sa-service-ready; do
    "$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra -Wno-deprecated-declarations \
        -I"$work/samgr-native/interfaces/innerkits/samgr_proxy/include" \
        -I"$headers/commonlibrary_c_utils-OpenHarmony-6.0-Release/base/include" \
        -I"$headers/communication_ipc-OpenHarmony-6.0-Release/interfaces/innerkits/ipc_core/include" \
        "$project/ports/tests/$name.cpp" \
        /mnt/harmonyos-source/system/system/lib64/platformsdk/libsamgr_proxy.z.so \
        /mnt/harmonyos-source/system/system/lib64/chipset-pub-sdk/libutils.z.so \
        -ldl -o "$out/$name"
done

python3 - "$out" <<'PY'
from pathlib import Path
import hashlib,sys
p=Path(sys.argv[1])
files=[f for f in sorted(p.iterdir()) if f.is_file() and not f.name.startswith('SHA256SUMS')]
(p/'SHA256SUMS').write_text(''.join(f'{hashlib.file_digest(f.open("rb"),"sha256").hexdigest()}  {f.name}\n' for f in files))
PY
printf 'Built OHOS diagnostics in %s\n' "$out"
