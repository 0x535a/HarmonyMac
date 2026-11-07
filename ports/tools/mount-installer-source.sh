#!/bin/bash
# Mount the L410 ISO, split SquashFS, and source ext4 images read-only in Linux.
# The argument must be a read-only ISO block device (for example an NBD export).
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

iso_device=${1:?usage: mount-installer-source.sh READ_ONLY_ISO_BLOCK_DEVICE}
base=/mnt/harmonyos-source
state=/run/harmonyos-source
map=harmonyos-source-payload
test "$(id -u)" = 0
test -b "$iso_device"
test "$(blockdev --getro "$iso_device")" = 1
test ! -e "$state"
mkdir -p "$state" "$base/iso" "$base/payload"
: > "$state/loops"
: > "$state/mounts"
printf '%s\n' "$iso_device" > "$state/iso-device"

mount -t iso9660 -o ro,nodev,nosuid,noexec "$iso_device" "$base/iso"
printf '%s\n' "$base/iso" >> "$state/mounts"
test "$(blkid -s LABEL -o value "$iso_device")" = L410_INSTALL

start=0
: > "$state/table"
parts=("$base/iso"/payload.parts/payload.squashfs.part-*)
test "${#parts[@]}" = 8
for part in "${parts[@]}"; do
    test -f "$part"
    loop=$(losetup --find --show --read-only "$part")
    printf '%s\n' "$loop" >> "$state/loops"
    sectors=$(blockdev --getsz "$loop")
    printf '%s %s linear %s 0\n' "$start" "$sectors" "$loop" >> "$state/table"
    start=$((start + sectors))
done
test "$start" = 28986440
dmsetup create "$map" --readonly < "$state/table"
printf '%s\n' "$map" > "$state/map"
test "$(blockdev --getro "/dev/mapper/$map")" = 1
mount -t squashfs -o ro,nodev,nosuid,noexec "/dev/mapper/$map" "$base/payload"
printf '%s\n' "$base/payload" >> "$state/mounts"

for pair in \
    system:p111-system.kr990-ext4-v1.img \
    vendor:p109-vendor.kr990-ext4-v1.img \
    sys_prod:p110-sys_prod.img \
    chip_prod:p108-chip_prod.kr990-ext4-v1.img \
    log:p009-log.img; do
    name=${pair%%:*}
    image=${pair#*:}
    loop=$(losetup --find --show --read-only "$base/payload/images/$image")
    printf '%s\n' "$loop" >> "$state/loops"
    mkdir -p "$base/$name"
    mount -t ext4 -o ro,noload,nodev,nosuid,noexec "$loop" "$base/$name"
    printf '%s\n' "$base/$name" >> "$state/mounts"
done
findmnt -R "$base" || findmnt | awk -v root="$base" 'index($0,root)'
printf 'Source mounts ready: %s (all read-only, no journal replay)\n' "$base"
