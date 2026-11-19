#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: build-keyboard-backlight.sh PROJECT WORK}")
work=$(realpath "${2:?}")
out=$work/generic-20261117
mkdir -p "$out"
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror -I"$project/ports/hardware" \
    "$project/ports/hardware/keyboard-backlight.c" "$project/ports/hardware/keyboard-backlight-cli.c" \
    -Wl,--no-undefined -o "$out/keyboard-backlight"
sha256sum "$out/keyboard-backlight"
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra -Werror -I"$project/ports/hardware" \
    "$project/ports/hardware/keyboard-backlight.c" "$project/ports/hardware/keyboard-light-hotkeys.c" \
    -ldl -Wl,--no-undefined -o "$out/keyboard-light-hotkeys"
sha256sum "$out/keyboard-light-hotkeys"
