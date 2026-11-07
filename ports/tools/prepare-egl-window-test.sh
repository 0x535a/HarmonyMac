#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: prepare-egl-window-test.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
graphics=$work/aurora-graphics-build
bash "$project/ports/tools/prepare-kms-test.sh" "$out" \
    "$work/static-bootstrap-test/root" "$work/kernel-test3-package" \
    "$work/core-closure2" "$graphics/libdisplay_buffer_vdi_impl.z.so" "$graphics/buffer-vdi-probe"
cp "$graphics/libEGL_impl.so" "$out/root/testroot/vendor/lib64/aurora-graphics/"
cp "$graphics/egl-window-probe" "$out/root/testroot/system/bin/aurora/"
python3 - "$out/root/aurora-init" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
script = path.read_text()
assert script.count('exec /system/bin/aurora/buffer-vdi-probe') == 1
script = script.replace('LD_LIBRARY_PATH=/system/lib64/aurora-mesa',
                        'LD_LIBRARY_PATH=/vendor/lib64/aurora-graphics:/system/lib64/aurora-mesa')
script = script.replace('exec /system/bin/aurora/buffer-vdi-probe', 'exec /system/bin/aurora/egl-window-probe')
script = script.replace('OHOS_KMS_TEST_', 'OHOS_EGL_WINDOW_TEST_')
path.write_text(script)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared EGL window fixture %s\n' "$out/initrd.img"
