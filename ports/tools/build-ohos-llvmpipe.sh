#!/bin/bash
# LLVM JIT software rendering, built for OHOS and isolated from the softpipe prefix.
set -euo pipefail
source=${1:?usage: build-ohos-llvmpipe.sh MESA_SOURCE WORK_DIRECTORY}
work=${2:?}
source=$(realpath "$source")
work=$(realpath "$work")
test -f "$work/llvm19-ohos-install/include/llvm/Config/llvm-config.h"
test -f "$work/llvm19-ohos-install/lib/libLLVM.so.19.1"
test -x "$work/ohos-llvm-config.py"
export OHOS_LLVM_PREFIX="$work/llvm19-ohos-install"
cross="$work/ohos-toolchain/ohos-llvmpipe-cross.ini"
python3 - "$work/ohos-toolchain/ohos-cross.ini" "$cross" "$work/ohos-llvm-config.py" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
s = s.replace('[binaries]', "[binaries]\nllvm-config = '" + sys.argv[3] + "'", 1)
Path(sys.argv[2]).write_text(s)
PY
meson setup "$work/mesa-ohos-llvmpipe-build" "$source" \
    --cross-file "$cross" --prefix=/system --libdir=lib64/aurora-mesa \
    --buildtype=release -Dcpp_rtti=false -Dauto_features=disabled -Dplatforms=[] \
    -Dgallium-drivers=llvmpipe -Dvulkan-drivers=[] \
    -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true \
    -Dglx=disabled -Dglvnd=disabled -Degl=enabled -Dgbm=enabled \
    -Dgles1=enabled -Dgles2=enabled -Dgallium-va=disabled \
    -Dshader-cache=disabled -Dvideo-codecs=[] \
    -Dc_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    -Dcpp_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    > "$work/mesa-ohos-llvmpipe-configure.log" 2>&1
ninja -C "$work/mesa-ohos-llvmpipe-build" -j3 > "$work/mesa-ohos-llvmpipe-build.log" 2>&1
DESTDIR="$work/llvmpipe-deps" meson install -C "$work/mesa-ohos-llvmpipe-build" --no-rebuild \
    > "$work/mesa-ohos-llvmpipe-install.log" 2>&1
printf 'OHOS llvmpipe installed under %s/llvmpipe-deps\n' "$work"
