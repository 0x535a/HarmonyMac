#!/bin/bash
# OHOS Mesa software rendering, optionally with Aurora Asahi in a separate build.
set -euo pipefail
source=${1:?usage: build-ohos-llvmpipe.sh MESA_SOURCE WORK_DIRECTORY [--asahi]}
work=${2:?}
source=$(realpath "$source")
work=$(realpath "$work")
test -f "$work/llvm19-ohos-install/include/llvm/Config/llvm-config.h"
test -f "$work/llvm19-ohos-install/lib/libLLVM.so.19.1"
test -x "$work/ohos-llvm-config.py"
export OHOS_LLVM_PREFIX="$work/llvm19-ohos-install"
profile=llvmpipe
drivers=llvmpipe
options=()
case "${3:-}" in
    '') ;;
    --asahi)
        profile=asahi
        drivers=asahi,llvmpipe
        # These build-machine compilers come from the same Mesa source as the
        # target driver. Their generated shader data is ABI-independent.
        native="$source/build/src"
        test -x "$native/compiler/clc/mesa_clc"
        test -x "$native/compiler/spirv/vtn_bindgen2"
        test -x "$native/asahi/clc/asahi_clc"
        export PATH="$native/compiler/clc:$native/compiler/spirv:$native/asahi/clc:$PATH"
        options+=(-Dmesa-clc=system -Dprecomp-compiler=system)
        ;;
    *) printf 'Unknown Mesa build option: %s\n' "$3" >&2; exit 2 ;;
esac
cross="$work/ohos-toolchain/ohos-$profile-cross.ini"
build="$work/mesa-ohos-$profile-build"
python3 - "$work/ohos-toolchain/ohos-cross.ini" "$cross" "$work/ohos-llvm-config.py" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
s = s.replace('[binaries]', "[binaries]\nllvm-config = '" + sys.argv[3] + "'", 1)
Path(sys.argv[2]).write_text(s)
PY
meson setup "$build" "$source" \
    --cross-file "$cross" --prefix=/system --libdir=lib64/aurora-mesa \
    --buildtype=release -Dcpp_rtti=false -Dauto_features=disabled -Dplatforms=[] \
    -Dgallium-drivers="$drivers" -Dvulkan-drivers=[] "${options[@]}" \
    -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true \
    -Dglx=disabled -Dglvnd=disabled -Degl=enabled -Dgbm=enabled \
    -Dgles1=enabled -Dgles2=enabled -Dgallium-va=disabled \
    -Dshader-cache=disabled -Dvideo-codecs=[] \
    -Dc_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    -Dcpp_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    > "$work/mesa-ohos-$profile-configure.log" 2>&1
ninja -C "$build" -j3 > "$work/mesa-ohos-$profile-build.log" 2>&1
DESTDIR="$work/$profile-deps" meson install -C "$build" --no-rebuild \
    > "$work/mesa-ohos-$profile-install.log" 2>&1
printf 'OHOS Mesa (%s) installed under %s/%s-deps\n' "$drivers" "$work" "$profile"
