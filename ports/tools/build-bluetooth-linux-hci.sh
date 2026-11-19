#!/bin/bash
set -euo pipefail
project=$(realpath "${1:?PROJECT}")
work=$(realpath "${2:?WORK}")
tree=$work/kernel8-network-build2/source
out=$work/bluetooth-20261117
mkdir -p "$out/vhci"
test "$(make -s -C "$tree" LLVM=-19 kernelrelease)" = 7.2.11-aurora-hmos-j514s-test8
sha256sum "$tree/.config" "$tree/Module.symvers" > "$out/vhci/kernel-input.sha256"
cp "$tree/drivers/bluetooth/hci_vhci.c" "$out/vhci/"
# BlueZ btvirt needs real kernel AF_ALG AES/CMAC interfaces. These diagnostic
# modules are built from the same tree, never enabled in the normal boot path.
cp "$tree/crypto/algif_hash.c" "$tree/crypto/algif_skcipher.c" "$out/vhci/"
printf 'obj-m += hci_vhci.o algif_hash.o algif_skcipher.o\n' > "$out/vhci/Makefile"
make -C "$tree" LLVM=-19 M="$out/vhci" -j2 modules > "$out/vhci/build.log" 2>&1
sha256sum -c "$out/vhci/kernel-input.sha256"
cc=$work/ohos-toolchain/ohos-cc
"$cc" -O2 -Wall -Wextra -Werror -std=gnu11 -fPIC -shared -fvisibility=hidden \
    "$project/ports/bluetooth/linux-hci-vendor.c" -lpthread -o "$out/libbt_vendor.z.so"
"$cc" -O2 -Wall -Wextra -Werror -std=gnu11 -I"$project/ports/bluetooth" \
    "$project/ports/tests/bluetooth-vendor-probe.c" -ldl -o "$out/bluetooth-vendor-probe"
"$cc" -O2 -Wall -Wextra -Werror -std=gnu11 \
    "$project/ports/tests/bluetooth-api-probe.c" -ldl -o "$out/bluetooth-api-probe"
sha256sum "$out/vhci/"*.ko "$out/libbt_vendor.z.so" "$out/bluetooth-vendor-probe"
