#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?usage: prepare-composer-test.sh PROJECT WORK OUTPUT}")
work=$(realpath "${2:?}")
out=${3:?}
graphics=$work/aurora-graphics-build
bash "$project/ports/tools/prepare-kms-test.sh" "$out" \
    "$work/static-bootstrap-test/root" "$work/kernel-test3-package" \
    "$work/core-closure2" "$graphics/libdisplay_buffer_vdi_impl.z.so" "$graphics/buffer-vdi-probe"
cp "$graphics/libdisplay_composer_vdi_impl.z.so" "$out/root/testroot/vendor/lib64/aurora-graphics/"
if ! test -d "$work/composer-service-closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" \
        /mnt/harmonyos-source "$work/composer-service-closure" /vendor/lib64/libdisplay_composer_service_1.3.z.so
fi
rsync -a "$work/composer-service-closure/." "$out/root/testroot/"
cp "$graphics/composer-probe" "$graphics/composer-service-probe" "$work/dmabuf-probe-ohos" "$out/root/testroot/system/bin/aurora/"
python3 - "$out/root/aurora-init" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
script = path.read_text()
old = "chroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH="
assert script.count(old) == 1
script = script.replace(old, "chroot /testroot /system/bin/aurora/dmabuf-probe-ohos\ndma_result=$?\necho OHOS_DMA_HEAP_RESULT=$dma_result\nchroot /testroot /system/bin/sh -c 'export LD_LIBRARY_PATH=/vendor/lib64/aurora-graphics:/system/lib64/aurora-mesa; exec /system/bin/aurora/composer-service-probe'\nservice_result=$?\necho OHOS_ORIGINAL_COMPOSER_RESULT=$service_result\n" + old)
script = script.replace("exec /system/bin/aurora/buffer-vdi-probe", "exec /system/bin/aurora/composer-probe")
script = script.replace('echo "OHOS_KMS_TEST_RESULT=$?"', 'composer_result=$?\necho OHOS_COMPOSER_RESULT=$composer_result\ncase "$dma_result:$service_result:$composer_result" in\n    0:0:0) echo OHOS_KMS_TEST_RESULT=0 ;;\n    *) echo OHOS_KMS_TEST_RESULT=1 ;;\nesac')
path.write_text(script)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared composer fixture %s\n' "$out/initrd.img"
