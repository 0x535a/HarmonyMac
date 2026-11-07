#!/bin/bash
# Cross-build the real LLVM JIT library for the OHOS ABI, isolated from glibc.
set -euo pipefail
source=${1:?usage: build-ohos-llvm.sh LLVM_SOURCE WORK_DIRECTORY}
work=${2:?}
source=$(realpath "$source")
work=$(realpath "$work")
test -f "$source/CMakeLists.txt"
test -x "$work/ohos-toolchain/ohos-cxx"
test -f "$work/ohos-llvm19-toolchain.cmake"
test -x /usr/bin/llvm-tblgen-19
build="$work/llvm19-ohos-build"
prefix="$work/llvm19-ohos-install"
cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$work/ohos-llvm19-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_RPATH=/system/lib64/aurora-mesa \
    -DCMAKE_C_FLAGS="-fno-emulated-tls -fPIC -Wno-unused-command-line-argument" \
    -DCMAKE_CXX_FLAGS="-fno-emulated-tls -fPIC -Wno-unused-command-line-argument" \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DLLVM_TABLEGEN=/usr/bin/llvm-tblgen-19 \
    -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-linux-ohos \
    -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-ohos \
    -DLLVM_ENABLE_PROJECTS='' -DLLVM_ENABLE_RUNTIMES='' \
    -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
    -DLLVM_DYLIB_COMPONENTS=all -DLLVM_ENABLE_PIC=ON \
    -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_THREADS=ON \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_PARALLEL_COMPILE_JOBS=2 -DLLVM_PARALLEL_LINK_JOBS=1 \
    > "$work/llvm19-ohos-configure.log" 2>&1
ninja -C "$build" -j2 LLVM > "$work/llvm19-ohos-build.log" 2>&1
# Install just the target shared library and the matching public/generated headers.
ninja -C "$build" install-LLVM install-llvm-headers \
    > "$work/llvm19-ohos-install.log" 2>&1
printf 'OHOS LLVM installed under %s\n' "$prefix"
