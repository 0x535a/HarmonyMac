#!/bin/bash
# Remove only resources recorded by mount-installer-source.sh.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
state=/run/harmonyos-source
test "$(id -u)" = 0
test -d "$state"
detach_below() {
    local prefix=$1 loop backing
    while IFS= read -r loop; do
        case "$loop" in /dev/loop[0-9]*) ;; *) exit 2;; esac
        backing=$(losetup -n -O BACK-FILE "$loop" 2>/dev/null || true)
        case "$backing" in "$prefix"/*) losetup --detach "$loop";; esac
    done < <(tac "$state/loops")
}
while IFS= read -r point; do
    case "$point" in /mnt/harmonyos-source/*) ;; *) exit 2;; esac
    # Configured loop devices hold their backing mount even after ext4 unmount.
    # Release each layer before attempting to unmount its backing filesystem.
    if [ "$point" = /mnt/harmonyos-source/payload ]; then
        detach_below "$point"
    elif [ "$point" = /mnt/harmonyos-source/iso ]; then
        test "$(cat "$state/map")" = harmonyos-source-payload
        if dmsetup info harmonyos-source-payload >/dev/null 2>&1; then
            dmsetup remove harmonyos-source-payload
        fi
        detach_below "$point"
    fi
    if mountpoint -q "$point"; then umount "$point"; fi
done < <(tac "$state/mounts")
if test -f "$state/map"; then
    test "$(cat "$state/map")" = harmonyos-source-payload
    if dmsetup info harmonyos-source-payload >/dev/null 2>&1; then
        dmsetup remove harmonyos-source-payload
    fi
fi
while IFS= read -r loop; do
    case "$loop" in /dev/loop[0-9]*) ;; *) exit 2;; esac
    if losetup "$loop" >/dev/null 2>&1; then losetup --detach "$loop"; fi
done < <(tac "$state/loops")
rm -rf "$state"
printf 'Read-only source mounts removed; NBD connection is managed separately.\n'
