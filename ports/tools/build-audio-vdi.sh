#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-audio-vdi.sh PROJECT WORK [--j514-experimental | --j514-fixture]}")
work=$(realpath "${2:?}")
out=$work/audio-service-20261115
includes=(-I"$project/ports/audio/include" -I"$out/include")
defines=()
case ${3:-} in
    "") ;;
    --j514-experimental) defines=(-DAURORA_J514_AUDIO_EXPERIMENTAL) ;;
    --j514-fixture) defines=(-DAURORA_J514_AUDIO_FIXTURE) ;;
    *) printf 'Unknown audio build profile: %s\n' "$3" >&2; exit 2 ;;
esac
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror -fPIC -shared \
    "${defines[@]}" "${includes[@]}" "$project/ports/audio/alsa-vdi.c" "$out/libs/libasound.so" -lm -pthread \
    -Wl,--no-undefined -Wl,--allow-shlib-undefined -Wl,-soname,libaudio_primary_impl.z.so \
    -Wl,-rpath,/vendor/lib64 -o "$out/libaudio_primary_impl.z.so" > "$out/vdi-build.log" 2>&1
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror "${includes[@]}" \
    "$project/ports/tests/audio-vdi-probe.c" -ldl -o "$out/audio-vdi-probe" >> "$out/vdi-build.log" 2>&1
if test "${3:-}" = --j514-experimental; then
    "$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror "${includes[@]}" \
        "$project/ports/tests/j514-vdi-probe.c" "$out/libs/libasound.so" -ldl -lm -pthread \
        -Wl,--allow-shlib-undefined -Wl,-rpath,/vendor/lib64 \
        -o "$out/j514-vdi-probe" >> "$out/vdi-build.log" 2>&1
fi
readelf -dW "$out/libaudio_primary_impl.z.so" > "$out/vdi-dynamic.txt"
sha256sum "$out/libaudio_primary_impl.z.so" "$out/audio-vdi-probe"
