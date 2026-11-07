#!/bin/bash
# Build the CPU GLES/EGL path against OHOS, not the Debian/glibc Mesa runtime.
set -euo pipefail
source=${1:?usage: build-ohos-softpipe.sh MESA_SOURCE WORK_DIRECTORY}
work=${2:?}
work=$(realpath "$work")
source=$(realpath "$source")
test -f "$work/ohos-toolchain/ohos-cross.ini"
test -f "$work/ohos-deps/system/lib64/aurora-mesa/pkgconfig/libdrm.pc"
test -f "$work/ohos-deps/system/lib64/aurora-mesa/pkgconfig/zlib.pc"
test -f "$work/ohos-deps/system/lib64/aurora-mesa/pkgconfig/expat.pc"
for patch_name in mesa-no-profiler-trace-scope mesa-ddebug-resource-release; do
    mesa_patch=$(realpath "$(dirname "$0")/../graphics/patches/$patch_name.patch")
    patch_log="$work/$patch_name-apply.log"
    if patch --dry-run -p1 -d "$source" < "$mesa_patch" > "$patch_log" 2>&1; then
        patch -p1 -d "$source" < "$mesa_patch" >> "$patch_log" 2>&1
    elif ! patch --dry-run -R -p1 -d "$source" < "$mesa_patch" >> "$patch_log" 2>&1; then
        echo "Mesa source differs from $patch_name; inspect $patch_log." >&2
        exit 1
    fi
done
meson setup "$work/mesa-ohos-softpipe-build" "$source" \
    --cross-file "$work/ohos-toolchain/ohos-cross.ini" \
    --prefix=/system --libdir=lib64/aurora-mesa --buildtype=release \
    -Dauto_features=disabled -Dplatforms=[] -Dgallium-drivers=softpipe \
    -Dvulkan-drivers=[] -Dllvm=disabled -Dglx=disabled -Dglvnd=disabled \
    -Degl=enabled -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled \
    -Dgallium-va=disabled -Dshader-cache=disabled -Dvideo-codecs=[] \
    -Dc_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    -Dcpp_args="-I$work/ohos-deps/system/include -fno-emulated-tls" \
    > "$work/mesa-ohos-softpipe-configure.log" 2>&1
ninja -C "$work/mesa-ohos-softpipe-build" -j3 > "$work/mesa-ohos-softpipe-build.log" 2>&1
DESTDIR="$work/ohos-deps" meson install -C "$work/mesa-ohos-softpipe-build" --no-rebuild \
    > "$work/mesa-ohos-softpipe-install.log" 2>&1
printf 'OHOS softpipe EGL/GLES installed under %s/ohos-deps\n' "$work"
